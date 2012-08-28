#include <iostream>
#include <stdlib.h>
#include <string>
#include <string.h>
#include <errno.h>
#include <sys/select.h>
#include <node.h>
#include <v8.h>

extern "C" {
#include <zephyr/zephyr.h>
}

// TODO: Yes, I know I should check my malloc()s.

using namespace v8;

namespace Zephyr {
    unsigned short port = 0;
    Z_AuthProc authentic = ZAUTH;
    Persistent<Function> on_msg;
}

uv_loop_t *Loop;

#define PROPERTY(name, value) target->Set(String::NewSymbol(#name), value)
#define METHOD(name) PROPERTY(name, FunctionTemplate::New(name)->GetFunction())

#define THROW(msg) { \
        ThrowException(Exception::Error(String::New(msg))); \
        return scope.Close(Undefined()); \
    }

char *getstr(const Local<Value>& value) {
    String::Utf8Value temp(value);
    char *ret = (char *) malloc(temp.length() + 1);
    strcpy(ret, *temp);
    return ret;
}

/*[ CHECK ]*******************************************************************/

void check_work(uv_work_t *req) {
    uv_async_t *async = (uv_async_t *) req->data;
    fd_set rfds;
    int fd = ZGetFD();
    while(true) {
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        if(select(fd + 1, &rfds, NULL, NULL, NULL) < 0) {
            perror("zephyr check");
            break;
        }
        uv_async_send(async);
    }
}

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
    EXTRACT(Date,   time,               z_time.tv_sec * 1000l);
    EXTRACT(Number, auth,               z_auth);
    
    struct hostent *host = (struct hostent *) gethostbyaddr(
        (char *) &notice->z_sender_addr, sizeof(struct in_addr), AF_INET);
    if(host && host->h_name) {
        PROPERTY(from_host, String::New(host->h_name));
    } else {
        PROPERTY(from_host, String::New(inet_ntoa(notice->z_sender_addr)));
    }
    
    if(notice->z_message_len > 0) {
        EXTRACT(String, signature, z_message);
        int sig_len = strlen(notice->z_message) + 1;
        if(sig_len >= notice->z_message_len) {
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
    
    if(notice->z_num_other_fields) {
        Local<Array> list = Array::New(notice->z_num_other_fields);
        for(int i = 0; i < notice->z_num_other_fields; ++i)
            list->Set(i, String::New(notice->z_other_fields[i]));
        PROPERTY(other_fields, list);
    }
}

#define REPORT(callback, error) { \
        Local<Value> argv[2] = { \
            Local<Value>::New(String::New(error)), \
            Local<Value>::New(Undefined()) \
        }; \
        callback->Call(Context::GetCurrent()->Global(), 2, argv); \
    }

void check_deliver(uv_async_t *async, int status) {
    Local<Function> callback = Local<Function>::New(Zephyr::on_msg);
    struct sockaddr_in from;
    ZNotice_t *notice;
    
    while(true) {
        int len = ZPending();
        if(len < 0) {
            REPORT(callback, strerror(errno));
            return;
        } else if(len == 0) {
            return;
        }
        
        notice = (ZNotice_t *) malloc(sizeof(ZNotice_t));
        if(ZReceiveNotice(notice, &from) != ZERR_NONE) {
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

void check_cleanup(uv_work_t *req) {
    uv_async_t *async = (uv_async_t *) req->data;
    uv_close((uv_handle_t*) &async, NULL);
    Zephyr::on_msg.Dispose();
    Zephyr::on_msg.Clear();
    delete async;
    delete req;
}

Handle<Value> check(const Arguments& args) {
    HandleScope scope;
    
    if(args.Length() != 1 || !args[0]->IsFunction())
        THROW("check(callback(err, msg))");
    if(!Zephyr::on_msg.IsEmpty())
        THROW("can't call check() while it's already running");
    
    uv_async_t *async = new uv_async_t;
    uv_work_t *req = new uv_work_t;
    req->data = (void *) async;
    Zephyr::on_msg = Persistent<Function>::New(Local<Function>::Cast(args[0]));
    uv_async_init(Loop, async, check_deliver);
    uv_queue_work(Loop, req, check_work, check_cleanup);
    return scope.Close(Undefined());
}

/*[ SUB ]*********************************************************************/

struct sub_baton {
    uv_work_t req;
    ZSubscription_t subscription;
    Persistent<Function> callback;
    Persistent<Value> arg;
};

void subscribe_work(uv_work_t *req) {
    sub_baton *data = (sub_baton *) req->data;
    ZSubscribeTo(&data->subscription, 1, Zephyr::port);
}

void subscribe_cleanup(uv_work_t *req) {
    sub_baton *data = (sub_baton *) req->data;
    Local<Function> callback = Local<Function>::New(data->callback);
    Local<Value> argv[1] = { Local<Value>::New(data->arg) };
    callback->Call(Context::GetCurrent()->Global(), 1, argv);
    
    data->callback.Dispose();
    data->arg.Dispose();
    free(data->subscription.zsub_recipient);
    free(data->subscription.zsub_classinst);
    free(data->subscription.zsub_class);
    delete data;
}

char subscribe_doc[] = "subscribe([ class, instance, recipient? ], callback)";

Handle<Value> subscribe(const Arguments& args) {
    HandleScope scope;
    
    if(args.Length() != 2 || !args[0]->IsArray() || !args[1]->IsFunction())
        THROW(subscribe_doc);
    Local<Array> sub = Local<Array>::Cast(args[0]);
    switch(sub->Length()) {
        case 3:
            if(!sub->Get(2)->IsString())
                THROW(subscribe_doc);
        case 2:
            if(!sub->Get(1)->IsString())
                THROW(subscribe_doc);
            if(!sub->Get(0)->IsString())
                THROW(subscribe_doc);
            break;
        default:
            THROW(subscribe_doc);
    }
    
    sub_baton *data = new sub_baton;
    data->req.data = (void *) data;
    data->callback = Persistent<Function>::New(Local<Function>::Cast(args[1]));
    data->arg = Persistent<Value>::New(sub);
    
    data->subscription.zsub_recipient = sub->Length() == 3 ?
        getstr(sub->Get(2)) : NULL;
    data->subscription.zsub_classinst = getstr(sub->Get(1));
    data->subscription.zsub_class = getstr(sub->Get(0));
    
    uv_queue_work(Loop, &data->req, subscribe_work, subscribe_cleanup);
    return scope.Close(Undefined());
}

/*[ MAIN ]********************************************************************/

void init(Handle<Object> target) {
    Loop = uv_default_loop();
    
    if(ZInitialize() != ZERR_NONE || ZOpenPort(&Zephyr::port) != ZERR_NONE) {
        // we should probably handle this better...
        perror("zephyr init");
        return;
    }
    
    PROPERTY(sender, String::New(ZGetSender()));
    PROPERTY(realm, String::New(ZGetRealm()));
    METHOD(check);
    METHOD(subscribe);
}

NODE_MODULE(zephyr, init)
