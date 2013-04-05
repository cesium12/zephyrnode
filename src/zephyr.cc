#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include <node.h>
#include <node_buffer.h>
#include <v8.h>

extern "C" {
#include <com_err.h>
#include <zephyr/zephyr.h>
}

using namespace v8;

namespace {

Persistent<Function> g_on_msg;

uv_loop_t *g_loop;

uv_poll_t g_zephyr_poll;

#define PROPERTY(name, value) target->Set(String::NewSymbol(#name), value)
#define METHOD(name) PROPERTY(name, FunctionTemplate::New(name)->GetFunction())

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

// XXX Yeah, I know I should check return values... but lazy...
char *getstr(const Handle<Value> str) {
  String::Utf8Value temp(Handle<String>::Cast(str));
  return strndup(*temp, temp.length());
}

Local<Object> ZUniqueIdToBuffer(const ZUnique_Id_t& uid) {
  return Local<Object>::New(
      node::Buffer::New(reinterpret_cast<const char*>(&uid),
                        sizeof(uid))->handle_);
}

/*[ CHECK ]*******************************************************************/

#define EXTRACT(type, name, field) PROPERTY(name, type::New(notice->field))

void ZephyrToObject(ZNotice_t *notice, Handle<Object> target) {
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

  PROPERTY(uid, ZUniqueIdToBuffer(notice->z_uid));
    
  struct hostent *host = (struct hostent *) gethostbyaddr(
      (char *) &notice->z_sender_addr, sizeof(struct in_addr), AF_INET);
  if (host && host->h_name) {
    PROPERTY(from_host, String::New(host->h_name));
  } else {
    PROPERTY(from_host, String::New(inet_ntoa(notice->z_sender_addr)));
  }

  // Split up the body's components by NULs.
  Local<Array> body = Array::New();
  for (int offset = 0, i = 0; offset < notice->z_message_len; i++) {
    const char* nul = static_cast<const char*>(
        memchr(notice->z_message + offset, 0, notice->z_message_len - offset));
    int nul_offset = nul ? (nul - notice->z_message) : notice->z_message_len;
    body->Set(i, String::New(notice->z_message + offset,
                             nul_offset - offset));
    offset = nul_offset + 1;
  }
  PROPERTY(body, body);
    
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
    ZephyrToObject(&notice, object);
    callback->Call(Context::GetCurrent()->Global(), 2, argv);
    ZFreeNotice(&notice);
  }
}

Handle<Value> setNoticeCallback(const Arguments& args) {
  HandleScope scope;
    
  if (args.Length() != 1 || !args[0]->IsFunction()) {
    ThrowException(Exception::TypeError(
        String::New("Parameter not a function")));
    return scope.Close(Undefined());
  }
    
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

Handle<Value> subscribeTo(const Arguments& args) {
  HandleScope scope;
    
  if (args.Length() != 1 || !args[0]->IsArray()) {
    ThrowException(Exception::TypeError(String::New("Invalid parameters")));
    return scope.Close(Undefined());
  }
    
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
      ThrowException(Exception::TypeError(
          String::New("Subs must be [ class, instance, recipient? ]")));
      return scope.Close(Undefined());
    }

    subs[i].zsub_recipient = sub->Length() == 3 ?
                             getstr(sub->Get(2)) : NULL;
    subs[i].zsub_classinst = getstr(sub->Get(1));
    subs[i].zsub_class = getstr(sub->Get(0));
  }


  int ret = ZSubscribeTo(subs, in_subs->Length(), 0);
    
  for (unsigned i = 0; i < in_subs->Length(); ++i) {
    free(subs[i].zsub_recipient);
    free(subs[i].zsub_classinst);
    free(subs[i].zsub_class);
  }
  delete[] subs;

  if (ret != ZERR_NONE) {
    ThrowException(ComErrException(ret));
    return scope.Close(Undefined());
  }
  return scope.Close(Undefined());
}

/*[ SEND ]********************************************************************/

std::string GetStringProperty(Handle<Object> source,
			      const char* key,
			      const char *default_value) {
  Local<Value> value = source->Get(String::New(key));
  if (value->IsUndefined())
    return default_value;
  String::Utf8Value value_utf8(source->Get(String::New(key)));
  return std::string(*value_utf8, value_utf8.length());
}

std::vector<ZUnique_Id_t> g_wait_on_uids;

Code_t SendFunction(ZNotice_t* notice, char* packet, int len, int waitforack) {
  // Send without blocking.
  Code_t ret = ZSendPacket(packet, len, 0);

  // Save the ZUnique_Id_t for waiting on. Arguably we do this better
  // than the real libzephyr; ZSendPacket doesn't get a notice
  // argument and parses the notice back out again.
  if (ret == ZERR_NONE && waitforack)
    g_wait_on_uids.push_back(notice->z_uid);

  return ret;
}

Handle<Value> sendNotice(const Arguments& args) {
  HandleScope scope;
    
  if (args.Length() != 1 || !args[0]->IsObject()) {
    ThrowException(Exception::TypeError(String::New("Notice must be object")));
    return scope.Close(Undefined());
  }

  // Pull fields out of the object.
  Local<Object> obj = Local<Object>::Cast(args[0]);

  // Assemble the body.
  std::string body;
  Local<Value> body_value = obj->Get(String::New("body"));
  if (body_value->IsArray()) {
    Local<Array> body_array = Local<Array>::Cast(body_value);
    for (uint32_t i = 0, len = body_array->Length(); i < len; i++) {
      String::Utf8Value value(body_array->Get(i));
      if (i > 0)
	body.push_back('\0');
      body.append(*value, value.length());
    }
  }

  std::string msg_class = GetStringProperty(obj, "class", "MESSAGE");
  std::string instance = GetStringProperty(obj, "instance", "PERSONAL");
  std::string format = GetStringProperty(obj, "format",
					 "http://zephyr.1ts.org/wiki/df");
  std::string opcode = GetStringProperty(obj, "opcode", "");
  std::string recipient = GetStringProperty(obj, "recipient", "");

  // Assemble the notice.
  ZNotice_t notice;
  memset(&notice, 0, sizeof(notice));
  notice.z_message_len = body.length();
  notice.z_message = const_cast<char*>(body.data());
  notice.z_kind = ACKED;
  notice.z_class = const_cast<char*>(msg_class.c_str());
  notice.z_class_inst = const_cast<char*>(instance.c_str());
  notice.z_default_format = const_cast<char*>(format.c_str());
  notice.z_opcode = const_cast<char*>(opcode.c_str());
  notice.z_recipient = const_cast<char*>(recipient.c_str());

  Code_t ret = ZSrvSendNotice(&notice, ZAUTH, SendFunction);

  if (ret != ZERR_NONE) {
    ThrowException(ComErrException(ret));
    g_wait_on_uids.clear();
    return scope.Close(Undefined());
  }

  Local<Array> uids = Array::New();
  for (unsigned i = 0; i < g_wait_on_uids.size(); i++) {
    uids->Set(i, ZUniqueIdToBuffer(g_wait_on_uids[i]));
  }
  g_wait_on_uids.clear();
  return scope.Close(uids);
}

/*[ SEND ]********************************************************************/

void Init(Handle<Object> target) {
  g_loop = uv_default_loop();

  if (ZInitialize() != ZERR_NONE || ZOpenPort(NULL) != ZERR_NONE) {
    // we should probably handle this better...
    perror("zephyr init");
    return;
  }

  InstallZephyrListener();
    
  PROPERTY(sender, String::New(ZGetSender()));
  PROPERTY(realm, String::New(ZGetRealm()));
  METHOD(setNoticeCallback);
  METHOD(subscribeTo);
  METHOD(sendNotice);
}

NODE_MODULE(zephyr, Init)

}  // namespace
