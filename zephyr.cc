#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <tuple>
#include <node.h>
#include <nan.h>
#include <uv.h>
#include <v8.h>

extern "C" {
#include <zephyr/zephyr.h>
}

using namespace v8;

namespace Zephyr {
    unsigned short port = 0;
    std::string msg;
    Nan::Persistent<Function> on_msg;
}

uv_loop_t *Loop;

#define STRING(value) Nan::New(value).ToLocalChecked()
#define PROPERTY(name, value) target->Set(STRING(#name), value)

#define CALL(func, ...) { \
    Nan::AsyncResource async("zephyr"); \
    constexpr int argc = \
        std::tuple_size<decltype(std::make_tuple(__VA_ARGS__))>::value; \
    Local<Value> argv[argc] = {__VA_ARGS__}; \
    async.runInAsyncScope( \
        Nan::GetCurrentContext()->Global(), func, argc, argv); \
}

#define CHECK(cond, error) \
    if(!(cond)) { \
        Nan::ThrowError(error); \
        return; \
    }

#define CHECK_CALL(call, action) { \
    long err = call; \
    if(err != ZERR_NONE) { \
        std::string msg(#call ": "); \
        msg += error_message(err); \
        action; \
        return; \
    } \
}

// XXX Unfortunately, it looks like everything except the select loop has to be
// XXX synchronous because the zephyr library isn't thread-safe. (Disclaimer: I
// XXX haven't investigated much.) Which is kind of bad. Really we should have
// XXX some sort of task queue in one background thread and use uv_queue_work
// XXX or something...
#define QUEUE(loop, req, work, cleanup) { \
    work(req); \
    cleanup(req); \
}

/*[ CHECK ]*******************************************************************/

void zephyr_to_object(ZNotice_t *notice, Local<Object> target) {
    PROPERTY(packet,           STRING(notice->z_packet));
    PROPERTY(version,          STRING(notice->z_version));
    PROPERTY(port,             Nan::New(notice->z_port));
    PROPERTY(checked_auth,     Nan::New(notice->z_checked_auth));
    PROPERTY(authent_len,      Nan::New(notice->z_authent_len));
    PROPERTY(ascii_authent,    STRING(notice->z_ascii_authent));
    PROPERTY(class,            STRING(notice->z_class));
    PROPERTY(instance,         STRING(notice->z_class_inst));
    PROPERTY(opcode,           STRING(notice->z_opcode));
    PROPERTY(sender,           STRING(notice->z_sender));
    PROPERTY(recipient,        STRING(notice->z_recipient));
    PROPERTY(format,           STRING(notice->z_default_format));
    PROPERTY(num_other_fields, Nan::New(notice->z_num_other_fields));
    PROPERTY(kind,             Nan::New(notice->z_kind));
    PROPERTY(auth,             Nan::New(notice->z_auth));
    PROPERTY(time,             Nan::New<Date>(notice->z_time.tv_sec * 1000.0)
                                   .ToLocalChecked());

    struct hostent *host = (struct hostent *) gethostbyaddr(
        (char *) &notice->z_sender_addr, sizeof(struct in_addr), AF_INET);
    if(host && host->h_name)
        PROPERTY(from_host, STRING(host->h_name));
    else
        PROPERTY(from_host, STRING(inet_ntoa(notice->z_sender_addr)));

    if(notice->z_message_len > 0) {
        PROPERTY(signature, STRING(notice->z_message));
        int sig_len = strlen(notice->z_message) + 1;
        if(sig_len >= notice->z_message_len) {
            PROPERTY(message, STRING(""));
        } else {
            char *message = strndup(notice->z_message + sig_len,
                                    notice->z_message_len - sig_len);
            PROPERTY(message, STRING(message));
            free(message);
        }
    } else {
        PROPERTY(signature, STRING(""));
        PROPERTY(message, STRING(""));
    }

    if(notice->z_num_other_fields) {
        Local<Array> list = Nan::New<Array>(notice->z_num_other_fields);
        for(int i = 0; i < notice->z_num_other_fields; ++i)
            list->Set(i, STRING(notice->z_other_fields[i]));
        PROPERTY(other_fields, list);
    }
}

void check_deliver(uv_async_t *async) {
    HandleScope scope(Isolate::GetCurrent());
    Local<Function> callback = Nan::New<Function>(Zephyr::on_msg);
    struct sockaddr_in from;
    ZNotice_t *notice;

    while(true) {
        int len = ZPending();
        if(len < 0) {
            CALL(callback, STRING(strerror(errno)), Nan::Undefined());
            return;
        } else if(len == 0) {
            return;
        }

        notice = (ZNotice_t *) malloc(sizeof(ZNotice_t));
        CHECK_CALL(
            ZReceiveNotice(notice, &from),
            CALL(callback, STRING(msg), Nan::Undefined()));

        Local<Object> object = Nan::New<Object>();
        zephyr_to_object(notice, object);
        CALL(callback, Nan::Undefined(), object);
        ZFreeNotice(notice);
    }
}

void check_work(uv_work_t *req) {
    uv_async_t *async = (uv_async_t *) req->data;
    fd_set rfds;
    int fd = ZGetFD();
    if(fd < 0) {
        Zephyr::msg = "ZGetFD: No current file descriptor";
        return;
    }
    while(true) {
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        if(select(fd + 1, &rfds, NULL, NULL, NULL) < 0) {
            Zephyr::msg = "select: ";
            Zephyr::msg += strerror(errno);
            return;
        }
        uv_async_send(async);
    }
}

void check_cleanup(uv_work_t *req, int status) {
    if(!Zephyr::msg.empty()) {
        Local<Function> callback = Nan::New<Function>(Zephyr::on_msg);
        CALL(callback, STRING(Zephyr::msg), Nan::Undefined());
        Zephyr::msg = "";
    }
    uv_async_t *async = (uv_async_t *) req->data;
    uv_close((uv_handle_t *) &async, NULL);
    Zephyr::on_msg.Reset();
    delete async;
    delete req;
}

NAN_METHOD(check) {
    CHECK(info.Length() == 1 && info[0]->IsFunction(),
        "check(callback(err, msg))");
    CHECK(Zephyr::on_msg.IsEmpty(), "check() is already running");

    uv_async_t *async = new uv_async_t;
    uv_work_t *req = new uv_work_t;
    req->data = (void *) async;
    Zephyr::on_msg.Reset(Local<Function>::Cast(info[0]));
    uv_async_init(Loop, async, check_deliver);
    uv_queue_work(Loop, req, check_work, check_cleanup);
}

/*[ SUBSCRIBE ]***************************************************************/

struct subscribe_baton {
    uv_work_t req;
    Nan::Persistent<Function> callback;
    ZSubscription_t *subs;
    int nsubs;
    std::string msg;
};

void subscribe_work(uv_work_t *req) {
    subscribe_baton *data = (subscribe_baton *) req->data;

    if(data->nsubs > 0) {
        CHECK_CALL(
            ZSubscribeToSansDefaults(data->subs, data->nsubs, Zephyr::port),
            data->msg = msg);
    }
}

void subscribe_cleanup(uv_work_t *req) {
    subscribe_baton *data = (subscribe_baton *) req->data;
    Local<Function> callback = Nan::New<Function>(data->callback);

    if(data->msg.empty()) {
        CALL(callback, Nan::Undefined());
    } else {
        CALL(callback, STRING(data->msg));
    }

    for(int i = 0; i < data->nsubs; ++i) {
        free(data->subs[i].zsub_recipient);
        free(data->subs[i].zsub_classinst);
        free(data->subs[i].zsub_class);
    }
    delete[] data->subs;
    data->callback.Reset();
    delete data;
}

// XXX Yeah, I know I should check return values... but lazy...
char *getstr(const Local<Value> str) {
    String::Utf8Value temp(Local<String>::Cast(str));
    return strndup(*temp, temp.length());
}

NAN_METHOD(subscribe) {
    CHECK(info.Length() == 2 && info[0]->IsArray() && info[1]->IsFunction(),
        "subscribe([ [ class, instance?, recipient? ], ... ], callback(err))");

    Local<Array> in_subs = Local<Array>::Cast(info[0]);
    ZSubscription_t *subs = new ZSubscription_t[in_subs->Length()];

    for(uint32_t i = 0; i < in_subs->Length(); ++i) {
        bool success = true;
        Local<Array> sub;
        if(!in_subs->Get(i)->IsArray()) {
            success = false;
        } else {
            sub = Local<Array>::Cast(in_subs->Get(i));
            switch(sub->Length()) {
                case 3:
                    if(!sub->Get(2)->IsString())
                        success = false;
                case 2:
                    if(!sub->Get(1)->IsString())
                        success = false;
                case 1:
                    if(!sub->Get(0)->IsString())
                        success = false;
                    break;
                default:
                    success = false;
            }
        }
        if(!success) {
            for(uint32_t j = 0; j < i; ++j) {
                free(subs[j].zsub_recipient);
                free(subs[j].zsub_classinst);
                free(subs[j].zsub_class);
            }
            delete[] subs;
            Nan::ThrowError("subs must be [ class, instance?, recipient? ]");
            return;
        }

        subs[i].zsub_recipient =
            sub->Length() > 2 ? getstr(sub->Get(2)) : strdup("*");
        subs[i].zsub_classinst =
            sub->Length() > 1 ? getstr(sub->Get(1)) : strdup("*");
        subs[i].zsub_class = getstr(sub->Get(0));
    }

    subscribe_baton *data = new subscribe_baton;
    data->req.data = (void *) data;
    data->callback.Reset(Local<Function>::Cast(info[1]));
    data->subs = subs;
    data->nsubs = in_subs->Length();
    QUEUE(Loop, &data->req, subscribe_work, subscribe_cleanup);
}

/*[ SUBS ]********************************************************************/

struct subs_baton {
    uv_work_t req;
    Nan::Persistent<Function> callback;
    ZSubscription_t *subs = NULL;
    int nsubs;
    std::string msg;
};

void subs_work(uv_work_t *req) {
    subs_baton *data = (subs_baton *) req->data;

    CHECK_CALL(
        ZRetrieveSubscriptions(Zephyr::port, &data->nsubs),
        data->msg = msg);

    data->subs = new ZSubscription_t[data->nsubs];
    for(int i = 0; i < data->nsubs; ++i) {
        int temp = 1;
        CHECK_CALL(
            ZGetSubscriptions(&(data->subs[i]), &temp),
            data->msg = msg);
    }
}

void subs_cleanup(uv_work_t *req) {
    subs_baton *data = (subs_baton *) req->data;
    Local<Function> callback = Nan::New<Function>(data->callback);

    if(data->msg.empty()) {
        Local<Array> subs = Nan::New<Array>(data->nsubs);
        for(int i = 0; i < data->nsubs; ++i) {
            Local<Array> sub = Nan::New<Array>(3);
            sub->Set(0, STRING(data->subs[i].zsub_class));
            sub->Set(1, STRING(data->subs[i].zsub_classinst));
            sub->Set(2, STRING(data->subs[i].zsub_recipient));
            subs->Set(i, sub);
        }
        CALL(callback, Nan::Undefined(), subs);
    } else {
        CALL(callback, STRING(data->msg), Nan::Undefined());
    }

    if(data->subs != NULL)
        delete[] data->subs;
    data->callback.Reset();
    delete data;
}

NAN_METHOD(subs) {
    CHECK(info.Length() == 1 && info[0]->IsFunction(),
        "subs(callback(err, [ sub, ... ]))");

    subs_baton *data = new subs_baton;
    data->req.data = (void *) data;
    data->callback.Reset(Local<Function>::Cast(info[0]));
    QUEUE(Loop, &data->req, subs_work, subs_cleanup);
}

/*[ SEND ]********************************************************************/

struct send_baton {
    uv_work_t req;
    Nan::Persistent<Function> callback;
    ZNotice_t *notice;
    std::string msg;
};

void send_work(uv_work_t *req) {
    send_baton *data = (send_baton *) req->data;
    CHECK_CALL(
        ZSendNotice(data->notice, ZAUTH),
        data->msg = msg);
}

void send_cleanup(uv_work_t *req) {
    send_baton *data = (send_baton *) req->data;
    Local<Function> callback = Nan::New<Function>(data->callback);
    if(data->msg.empty()) {
        CALL(callback, Nan::Undefined());
    } else {
        CALL(callback, STRING(data->msg));
    }
    
    free(data->notice->z_message);
    free(data->notice->z_class);
    free(data->notice->z_class_inst);
    free(data->notice->z_opcode);
    free(data->notice->z_recipient);
    free(data->notice->z_sender);
    delete data->notice;
    data->callback.Reset();
    delete data;
}

char *mkstr(Local<Object> source, const char *key, const char *def) {
    return source->Has(STRING(key)) ?
        getstr(source->Get(STRING(key))->ToString()) :
        strdup(def);
}

// XXX this is terrible
void object_to_zephyr(Local<Object> source, ZNotice_t *notice) {
    char *signature = mkstr(source, "signature", "");
    char *message   = mkstr(source, "message", "");
    notice->z_message_len = strlen(signature) + strlen(message) + 2;
    notice->z_message = (char *) malloc(notice->z_message_len);
    strcpy(notice->z_message, signature);
    strcpy(notice->z_message + strlen(signature) + 1, message);
    free(signature);
    free(message);
    notice->z_kind       = ACKED;
    notice->z_class      = mkstr(source, "class", "MESSAGE");
    notice->z_class_inst = mkstr(source, "instance", "PERSONAL");
    notice->z_opcode     = mkstr(source, "opcode", "");
    notice->z_recipient  = mkstr(source, "recipient", "");
    notice->z_sender     = mkstr(source, "sender", "");
}

NAN_METHOD(send) {
    CHECK(info.Length() == 2 && info[0]->IsObject() && info[1]->IsFunction(),
        "subscribe({ ... }, callback(err))");

    send_baton *data = new send_baton;
    data->req.data = (void *) data;
    data->callback.Reset(Local<Function>::Cast(info[1]));
    data->notice = new ZNotice_t();
    object_to_zephyr(Local<Object>::Cast(info[0]), data->notice);
    QUEUE(Loop, &data->req, send_work, send_cleanup);
}

/*[ INIT ]********************************************************************/

NAN_MODULE_INIT(init) {
    Loop = uv_default_loop();

    CHECK_CALL(ZInitialize(), Nan::ThrowError(msg.c_str()));
    CHECK_CALL(ZOpenPort(&Zephyr::port), Nan::ThrowError(msg.c_str()));

    PROPERTY(sender, STRING(ZGetSender()));
    PROPERTY(realm, STRING(ZGetRealm()));
    NAN_EXPORT(target, check);
    NAN_EXPORT(target, subscribe);
    NAN_EXPORT(target, subs);
    NAN_EXPORT(target, send);
}

NODE_MODULE(zephyr, init)
