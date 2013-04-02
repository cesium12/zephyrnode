#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <node.h>
#include <v8.h>

extern "C" {
#include <zephyr/zephyr.h>
}

using namespace v8;

namespace {

unsigned short g_port = 0;
Z_AuthProc g_authentic = ZAUTH;
Persistent<Function> g_on_msg;

uv_loop_t *g_loop;

uv_poll_t g_zephyr_poll;

}  // namespace


#define PROPERTY(name, value) target->Set(String::NewSymbol(#name), value)
#define METHOD(name) PROPERTY(name, FunctionTemplate::New(name)->GetFunction())

#define THROW(msg) { \
        ThrowException(Exception::Error(String::New(msg))); \
        return scope.Close(Undefined()); \
    }

#define REPORT(callback, error) { \
        Local<Value> argv[2] = { \
            Local<Value>::New(String::New(error)), \
            Local<Value>::New(Undefined()) \
        }; \
        callback->Call(Context::GetCurrent()->Global(), 2, argv); \
    }

// XXX Unfortunately, it looks like everything except the select loop has to be
// XXX synchronous because the zephyr library isn't thread-safe. (Disclaimer: I
// XXX haven't investigated much.) Which is kind of bad. Really we should have
// XXX some sort of task queue in one background thread and use uv_queue_work
// XXX or something...
#define QUEUE(loop, req, work, cleanup) {	\
    work(req); \
    cleanup(req); \
}

// XXX Yeah, I know I should check return values... but lazy...
char *getstr(const Handle<Value> str) {
  String::Utf8Value temp(Handle<String>::Cast(str));
  return strndup(*temp, temp.length());
}

/*[ CHECK ]*******************************************************************/

#define EXTRACT(type, name, field) PROPERTY(name, type::New(notice->field))

void zephyr_to_object(ZNotice_t *notice, Handle<Object> target) {
  EXTRACT(String, packet,             z_packet);
  EXTRACT(String, version,            z_version);
  EXTRACT(Number, port,               z_port);
  EXTRACT(Number, checked_auth,       z_checked_auth);
  EXTRACT(Number, authent_len,        z_authent_len);
  EXTRACT(String, ascii_authent,      z_ascii_authent);
  EXTRACT(String, class,              z_class);
  EXTRACT(String, instance,           z_class_inst);
  EXTRACT(String, opcode,             z_opcode);
  EXTRACT(String, sender,             z_sender);
  EXTRACT(String, recipient,          z_recipient);
  EXTRACT(String, format,             z_default_format);
  EXTRACT(Number, num_other_fields,   z_num_other_fields);
  EXTRACT(Number, kind,               z_kind);
  EXTRACT(Date,   time,               z_time.tv_sec * 1000.0);
  EXTRACT(Number, auth,               z_auth);
    
  struct hostent *host = (struct hostent *) gethostbyaddr(
      (char *) &notice->z_sender_addr, sizeof(struct in_addr), AF_INET);
  if (host && host->h_name) {
    PROPERTY(from_host, String::New(host->h_name));
  } else {
    PROPERTY(from_host, String::New(inet_ntoa(notice->z_sender_addr)));
  }
    
  if (notice->z_message_len > 0) {
    EXTRACT(String, signature, z_message);
    int sig_len = strlen(notice->z_message) + 1;
    if (sig_len >= notice->z_message_len) {
      PROPERTY(message, String::New(""));
    } else {
      char *message = strndup(notice->z_message + sig_len,
                              notice->z_message_len - sig_len);
      PROPERTY(message, String::New(message));
      free(message);
    }
  } else {
    PROPERTY(signature, String::New(""));
    PROPERTY(message, String::New(""));
  }
    
  if (notice->z_num_other_fields) {
    Local<Array> list = Array::New(notice->z_num_other_fields);
    for (int i = 0; i < notice->z_num_other_fields; ++i)
      list->Set(i, String::New(notice->z_other_fields[i]));
    PROPERTY(other_fields, list);
  }
}

void OnZephyrFDReady(uv_poll_t* handle, int status, int events) {
  Local<Function> callback = Local<Function>::New(g_on_msg);
  struct sockaddr_in from;
  ZNotice_t *notice;
    
  while (true) {
    int len = ZPending();
    if (len < 0) {
      REPORT(callback, strerror(errno));
      return;
    } else if (len == 0) {
      return;
    }
        
    notice = (ZNotice_t *) malloc(sizeof(ZNotice_t));
    if (!notice || ZReceiveNotice(notice, &from) != ZERR_NONE) {
      REPORT(callback, "error receiving zephyrgram");
      return;
    }

    Handle<Object> object = Object::New();
    Local<Value> argv[2] = {
      Local<Value>::New(Undefined()),
      Local<Object>::New(object)
    };
    zephyr_to_object(notice, object);
    callback->Call(Context::GetCurrent()->Global(), 2, argv);
    ZFreeNotice(notice);
  }
}

Handle<Value> setMessageCallback(const Arguments& args) {
  HandleScope scope;
    
  if (args.Length() != 1 || !args[0]->IsFunction())
    THROW("setMessageCallback(callback(err, msg))");
    
  g_on_msg = Persistent<Function>::New(Local<Function>::Cast(args[0]));

  return scope.Close(Undefined());
}

void InstallZephyrListener() {
  int fd = ZGetFD();
  if (fd < 0) {
    fprintf(stderr, "No zephyr FD\n");
    return;
  }

  int ret;
  ret = uv_poll_init(g_loop, &g_zephyr_poll, fd);
  if (ret != 0) {
    fprintf(stderr, "uv_poll_init: %d\n", ret);
    return;
  }

  ret = uv_poll_start(&g_zephyr_poll, UV_READABLE, OnZephyrFDReady);
  if (ret != 0) {
    fprintf(stderr, "uv_poll_start: %d\n", ret);
    return;
  }
}

/*[ SUB ]*********************************************************************/

struct subscribe_baton {
  uv_work_t req;
  Persistent<Function> callback;
  ZSubscription_t *subs;
  Persistent<Array> out_subs;
};

void subscribe_work(uv_work_t *req) {
  subscribe_baton *data = (subscribe_baton *) req->data;
  int length = data->out_subs->Length();
    
  if (length > 0)
    ZSubscribeTo(data->subs, length, g_port);
    
  for (int i = 0; i < length; ++i) {
    free(data->subs[i].zsub_recipient);
    free(data->subs[i].zsub_classinst);
    free(data->subs[i].zsub_class);
  }
  delete[] data->subs;
}

void subscribe_cleanup(uv_work_t *req) {
  subscribe_baton *data = (subscribe_baton *) req->data;
  Local<Function> callback = Local<Function>::New(data->callback);
  Local<Value> argv[1] = { Local<Value>::New(data->out_subs) };
  callback->Call(Context::GetCurrent()->Global(), 1, argv);
    
  data->callback.Dispose();
  data->out_subs.Dispose();
  delete data;
}

Handle<Value> subscribe(const Arguments& args) {
  HandleScope scope;
    
  if (args.Length() != 2 || !args[0]->IsArray() || !args[1]->IsFunction())
    THROW("subscribe([ [ class, instance, recipient? ], ... ], callback)");
    
  Local<Array> in_subs = Local<Array>::Cast(args[0]);
  Local<Array> out_subs = Array::New(in_subs->Length());
  ZSubscription_t *subs = new ZSubscription_t[in_subs->Length()];
    
  for (uint32_t i = 0; i < in_subs->Length(); ++i) {
    bool success = true;
    Local<Array> sub;
    if (!in_subs->Get(i)->IsArray()) {
      success = false;
    } else {
      sub = Local<Array>::Cast(in_subs->Get(i));
      switch (sub->Length()) {
        case 3:
          if (!sub->Get(2)->IsString())
            success = false;
        case 2:
          if (!sub->Get(1)->IsString())
            success = false;
          if (!sub->Get(0)->IsString())
            success = false;
          break;
        default:
          success = false;
      }
    }
    if (!success) {
      delete[] subs;
      THROW("subs must be [ class, instance, recipient? ]");
    }

    subs[i].zsub_recipient = sub->Length() == 3 ?
                             getstr(sub->Get(2)) : NULL;
    subs[i].zsub_classinst = getstr(sub->Get(1));
    subs[i].zsub_class = getstr(sub->Get(0));
    out_subs->Set(i, sub);
  }
    
  subscribe_baton *data = new subscribe_baton;
  data->req.data = (void *) data;
  data->callback = Persistent<Function>::New(Local<Function>::Cast(args[1]));
  data->subs = subs;
  data->out_subs = Persistent<Array>::New(out_subs);
  QUEUE(g_loop, &data->req, subscribe_work, subscribe_cleanup);
  return scope.Close(Undefined());
}

/*[ SUBS ]********************************************************************/

struct subs_baton {
  uv_work_t req;
  Persistent<Function> callback;
  ZSubscription_t *subs;
  int nsubs;
};

void subs_work(uv_work_t *req) {
  subs_baton *data = (subs_baton *) req->data;
    
  if (ZRetrieveSubscriptions(g_port, &data->nsubs) != ZERR_NONE)
    return;
    
  data->subs = new ZSubscription_t[data->nsubs];
  for (int i = 0; i < data->nsubs; ++i) {
    int temp = 1;
    if (ZGetSubscriptions(&(data->subs[i]), &temp) != ZERR_NONE) {
      delete[] data->subs;
      data->subs = NULL;
      return;
    }
  }
}

void subs_cleanup(uv_work_t *req) {
  subs_baton *data = (subs_baton *) req->data;
    
  Local<Function> callback = Local<Function>::New(data->callback);
  if (data->subs == NULL) {
    REPORT(callback, "couldn't get subs");
  } else {
    Handle<Array> subs = Array::New(data->nsubs);
    for (int i = 0; i < data->nsubs; ++i) {
      Handle<Array> sub = Array::New(3);
      sub->Set(0, String::New(data->subs[i].zsub_class));
      sub->Set(1, String::New(data->subs[i].zsub_classinst));
      sub->Set(2, String::New(data->subs[i].zsub_recipient));
      subs->Set(i, sub);
    }
    Local<Value> argv[2] = {
      Local<Value>::New(Undefined()),
      Local<Value>::New(subs)
    };
    callback->Call(Context::GetCurrent()->Global(), 2, argv);
    delete[] data->subs;
  }
    
  data->callback.Dispose();
  delete data;
}

Handle<Value> subs(const Arguments& args) {
  HandleScope scope;
    
  if (args.Length() != 1 || !args[0]->IsFunction())
    THROW("subs(callback(err, [ sub, ... ]))");
    
  subs_baton *data = new subs_baton;
  data->req.data = (void *) data;
  data->callback = Persistent<Function>::New(Local<Function>::Cast(args[0]));
  data->subs = NULL;
  QUEUE(g_loop, &data->req, subs_work, subs_cleanup);
  return scope.Close(Undefined());
}

/*[ SEND ]********************************************************************/

struct send_baton {
  uv_work_t req;
  Persistent<Function> callback;
  ZNotice_t *notice;
  bool error;
};

void send_work(uv_work_t *req) {
  send_baton *data = (send_baton *) req->data;
    
  data->error = ZSendNotice(data->notice, ZAUTH) != ZERR_NONE;
    
  free(data->notice->z_message);
  free(data->notice->z_class);
  free(data->notice->z_class_inst);
  free(data->notice->z_default_format);
  free(data->notice->z_opcode);
  free(data->notice->z_recipient);
  delete data->notice;
}

void send_cleanup(uv_work_t *req) {
  send_baton *data = (send_baton *) req->data;
  Local<Function> callback = Local<Function>::New(data->callback);
  Local<Value> argv[1] = { Local<Value>::New(
      data->error ? String::New("failed to send zephyr") : Undefined()) };
  callback->Call(Context::GetCurrent()->Global(), 1, argv);
    
  data->callback.Dispose();
  delete data;
}

char *mkstr(Handle<Object> source, const char *key, const char *def) {
  return source->Has(String::New(key)) ?
      getstr(source->Get(String::New(key))->ToString()) :
      strdup(def);
}

// XXX this is terrible
void object_to_zephyr(Handle<Object> source, ZNotice_t *notice) {
  char *signature = mkstr(source, "signature", "");
  char *message   = mkstr(source, "message", "");
  notice->z_message_len = strlen(signature) + strlen(message) + 2;
  notice->z_message = (char *) malloc(notice->z_message_len);
  strcpy(notice->z_message, signature);
  strcpy(notice->z_message + strlen(signature) + 1, message);
  free(signature);
  free(message);
  notice->z_kind           = ACKED;
  notice->z_class          = mkstr(source, "class", "MESSAGE");
  notice->z_class_inst     = mkstr(source, "instance", "PERSONAL");
  notice->z_default_format = mkstr(source, "format", "");
  notice->z_opcode         = mkstr(source, "opcode", "");
  notice->z_recipient      = mkstr(source, "recipient", "");
}

Handle<Value> send(const Arguments& args) {
  HandleScope scope;
    
  if (args.Length() != 2 || !args[0]->IsObject() || !args[1]->IsFunction())
    THROW("subscribe({ ... }, callback(err))");
    
  send_baton *data = new send_baton;
  data->req.data = (void *) data;
  data->callback = Persistent<Function>::New(Local<Function>::Cast(args[1]));
  data->error = false;
  data->notice = new ZNotice_t();
  object_to_zephyr(Local<Object>::Cast(args[0]), data->notice);
  QUEUE(g_loop, &data->req, send_work, send_cleanup);
  return scope.Close(Undefined());
}

/*[ SEND ]********************************************************************/

void init(Handle<Object> target) {
  g_loop = uv_default_loop();
    
  if (ZInitialize() != ZERR_NONE || ZOpenPort(&g_port) != ZERR_NONE) {
    // we should probably handle this better...
    perror("zephyr init");
    return;
  }

  InstallZephyrListener();
    
  PROPERTY(sender, String::New(ZGetSender()));
  PROPERTY(realm, String::New(ZGetRealm()));
  METHOD(setMessageCallback);
  METHOD(subscribe);
  METHOD(subs);
  METHOD(send);
}

NODE_MODULE(zephyr, init)
