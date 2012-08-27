#include <stdlib.h>
#include <string>
#include <string.h>
#include <node.h>
#include <v8.h>

extern "C" {
#include <zephyr/zephyr.h>
}

struct zobj {
    unsigned short port;
    Z_AuthProc authentic;
    int blocking;
} Zephyr;

uv_loop_t *Loop;

using namespace v8;

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

struct check_baton {
    uv_work_t req;
    Persistent<Function> callback;
};

Handle<Value> getfd(const Arguments& args) {
    HandleScope scope;
    return scope.Close(Number::New(ZGetFD()));
}

struct sub_baton {
    uv_work_t req;
    ZSubscription_t subscription;
    Persistent<Function> callback;
    Persistent<Value> arg;
};

void subscribe_callback(uv_work_t *req) {
    sub_baton *data = (sub_baton *) req->data;
    ZSubscribeToSansDefaults(&data->subscription, 1, Zephyr.port);
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
    free(data);
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
    
    sub_baton *data = new sub_baton();
    data->req.data = (void *) data;
    data->callback = Persistent<Function>::New(Local<Function>::Cast(args[1]));
    data->arg = Persistent<Value>::New(sub);
    
    data->subscription.zsub_recipient = sub->Length() == 3 ?
        getstr(sub->Get(2)) : NULL;
    data->subscription.zsub_classinst = getstr(sub->Get(1));
    data->subscription.zsub_class = getstr(sub->Get(0));
    
    uv_queue_work(Loop, &data->req, subscribe_callback, subscribe_cleanup);
    return scope.Close(Undefined());
}

#define METHOD(name) \
    target->Set(String::NewSymbol(#name), \
        FunctionTemplate::New(name)->GetFunction())

void init(Handle<Object> target) {
    Loop = uv_default_loop();
    
    Zephyr.authentic = ZAUTH;
    Zephyr.port = 0;
    Zephyr.blocking = 0;
    
    if(ZOpenPort(&Zephyr.port) != ZERR_NONE)
        // we should probably handle this better...
        return;
    
    METHOD(subscribe);
}

NODE_MODULE(zephyr, init)
