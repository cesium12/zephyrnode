#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <node.h>
#include <v8.h>

extern "C" {
#include <com_err.h>
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

Local<Value> ComErrException(Code_t code) {
  const char* msg = error_message(code);
  Local<Value> err = Exception::Error(String::New(msg));
  Local<Object> obj = err->ToObject();
  obj->Set(String::NewSymbol("code"), Integer::New(code));
  return err;
}

void CallWithError(Handle<Function> callback, Code_t code) {
  Local<Value> err = ComErrException(code);
  callback->Call(Context::GetCurrent()->Global(), 1, &err);
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
  ZNotice_t notice;
    
  while (true) {
    int len = ZPending();
    if (len < 0) {
      CallWithError(callback, errno);
      return;
    } else if (len == 0) {
      return;
    }

    int ret = ZReceiveNotice(&notice, &from);
    if (ret != ZERR_NONE) {
      CallWithError(callback, ret);
      return;
    }

    Handle<Object> object = Object::New();
    Local<Value> argv[2] = {
      Local<Value>::New(Null()),
      Local<Object>::New(object)
    };
    zephyr_to_object(&notice, object);
    callback->Call(Context::GetCurrent()->Global(), 2, argv);
    ZFreeNotice(&notice);
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
  int length;
  ZSubscription_t *subs;
  Code_t ret;
};

void subscribe_work(uv_work_t *req) {
  subscribe_baton *data = (subscribe_baton *) req->data;

  if (data->length > 0)
    data->ret = ZSubscribeTo(data->subs, data->length, g_port);
  else
    data->ret = ZERR_NONE;
    
  for (int i = 0; i < data->length; ++i) {
    free(data->subs[i].zsub_recipient);
    free(data->subs[i].zsub_classinst);
    free(data->subs[i].zsub_class);
  }
  delete[] data->subs;
}

void subscribe_cleanup(uv_work_t *req) {
  HandleScope scope;

  subscribe_baton *data = (subscribe_baton *) req->data;
  Local<Value> arg;
  if (data->ret != ZERR_NONE) {
    arg = ComErrException(data->ret);
  } else {
    arg = Local<Value>::New(Null());
  }
  data->callback->Call(Context::GetCurrent()->Global(), 1, &arg);
    
  data->callback.Dispose();
  delete data;
}

Handle<Value> subscribeTo(const Arguments& args) {
  HandleScope scope;
    
  if (args.Length() != 2 || !args[0]->IsArray() || !args[1]->IsFunction())
    THROW("subscribe([ [ class, instance, recipient? ], ... ], callback)");
    
  Local<Array> in_subs = Local<Array>::Cast(args[0]);
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
  }
    
  subscribe_baton *data = new subscribe_baton;
  data->req.data = (void *) data;
  data->callback = Persistent<Function>::New(Local<Function>::Cast(args[1]));
  data->length = in_subs->Length();
  data->subs = subs;
  QUEUE(g_loop, &data->req, subscribe_work, subscribe_cleanup);
  return scope.Close(Undefined());
}

/*[ SUBS ]********************************************************************/

struct subs_baton {
  uv_work_t req;
  Persistent<Function> callback;
  Code_t ret;
  ZSubscription_t *subs;
  int nsubs;
};

void subs_work(uv_work_t *req) {
  subs_baton *data = (subs_baton *) req->data;

  data->ret = ZRetrieveSubscriptions(g_port, &data->nsubs);
  if (data->ret != ZERR_NONE)
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
  HandleScope scope;

  subs_baton *data = (subs_baton *) req->data;
    
  if (data->ret != ZERR_NONE) {
    CallWithError(data->callback, data->ret);
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
      Local<Value>::New(Null()),
      Local<Value>::New(subs)
    };
    data->callback->Call(Context::GetCurrent()->Global(), 2, argv);
  }
    
  data->callback.Dispose();
  delete[] data->subs;
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

Handle<Value> sendNotice(const Arguments& args) {
  HandleScope scope;
    
  if (args.Length() != 1 || !args[0]->IsObject())
    THROW("sendNotice({ ... })");

  ZNotice_t notice;
  memset(&notice, 0, sizeof(notice));
  object_to_zephyr(Local<Object>::Cast(args[0]), &notice);

  int ret = ZSendNotice(&notice, ZAUTH);

  // FIXME: This is silly.
  free(notice.z_message);
  free(notice.z_class);
  free(notice.z_class_inst);
  free(notice.z_default_format);
  free(notice.z_opcode);
  free(notice.z_recipient);

  if (ret != ZERR_NONE) {
    ThrowException(ComErrException(ret));
    return scope.Close(Undefined());
  }

  // TODO: We'll return other stuff later.
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
  METHOD(subscribeTo);
  METHOD(subs);
  METHOD(sendNotice);
}

NODE_MODULE(zephyr, init)
