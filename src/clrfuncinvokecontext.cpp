#include "owin.h"

void completeOnV8Thread(uv_async_t* handle, int status)
{
    if (debugMode) 
        System::Console::WriteLine("completeOnV8Thread");

    HandleScope handleScope;
    uv_owin_async_t* uv_owin_async = CONTAINING_RECORD(handle, uv_owin_async_t, uv_async);
    System::Object^ context = uv_owin_async->context;
    (dynamic_cast<ClrFuncInvokeContext^>(context))->CompleteOnV8Thread();
}

void callFuncOnV8Thread(uv_async_t* handle, int status)
{
    if (debugMode) 
        System::Console::WriteLine("continueOnCLRThread");

    HandleScope handleScope;
    uv_owin_async_t* uv_owin_async = CONTAINING_RECORD(handle, uv_owin_async_t, uv_async);
    System::Object^ context = uv_owin_async->context;
    (dynamic_cast<NodejsFuncInvokeContext^>(context))->CallFuncOnV8Thread();
}

ClrFuncInvokeContext::ClrFuncInvokeContext(Handle<Function> callback)
{
    this->callback = new Persistent<Function>;
    *(this->callback) = Persistent<Function>::New(callback);
    this->uv_owin_async = new uv_owin_async_t;
    this->uv_owin_async->context = this;
    uv_async_init(uv_default_loop(), &this->uv_owin_async->uv_async, completeOnV8Thread);
    this->funcWaitHandle = gcnew AutoResetEvent(false);
    this->uv_owin_async_func = NULL;
    this->RecreateUvOwinAsyncFunc();
}

ClrFuncInvokeContext::~ClrFuncInvokeContext()
{
    if (debugMode)
        System::Console::WriteLine("~ClrFuncInvokeContext");
    
    this->!ClrFuncInvokeContext();
}

ClrFuncInvokeContext::!ClrFuncInvokeContext()
{
    if (debugMode)
        System::Console::WriteLine("!ClrFuncInvokeContext");

    this->DisposeCallback();
    this->DisposeUvOwinAsync();
    this->DisposeUvOwinAsyncFunc();
}

void ClrFuncInvokeContext::RecreateUvOwinAsyncFunc()
{
    this->DisposeUvOwinAsyncFunc();
    this->uv_owin_async_func = new uv_owin_async_t;
    uv_async_init(uv_default_loop(), &this->uv_owin_async_func->uv_async, callFuncOnV8Thread);
    // release one CLR thread associated with this call from JS to CLR 
    // that waits to call back to an exported JS function 
    this->funcWaitHandle->Set(); 
}

uv_owin_async_t* ClrFuncInvokeContext::WaitForUvOwinAsyncFunc()
{
    this->funcWaitHandle->WaitOne();
    return this->uv_owin_async_func;
}

void ClrFuncInvokeContext::DisposeCallback()
{
    if (this->callback)
    {
        if (debugMode)
            System::Console::WriteLine("Disposing callback");

        (*(this->callback)).Dispose();
        (*(this->callback)).Clear();
        delete this->callback;
        this->callback = NULL;        
    }
}

void ClrFuncInvokeContext::DisposeUvOwinAsync()
{
    if (this->uv_owin_async)
    {
        if (debugMode)
            System::Console::WriteLine("Disposing uv_owin_async");

        uv_unref((uv_handle_t*)&this->uv_owin_async->uv_async);
        delete this->uv_owin_async;
        this->uv_owin_async = NULL;
    }
}

void ClrFuncInvokeContext::DisposeUvOwinAsyncFunc()
{
    if (this->uv_owin_async_func)
    {
        if (debugMode)
            System::Console::WriteLine("Disposing uv_owin_async_func");

        uv_unref((uv_handle_t*)&this->uv_owin_async_func->uv_async);
        delete this->uv_owin_async_func;
        this->uv_owin_async_func = NULL;
    }
}

void ClrFuncInvokeContext::CompleteOnCLRThread(Task<System::Object^>^ task)
{
    if (debugMode)
        System::Console::WriteLine("CompleteOnCLRThread");

    // TODO: what prevents GC collection of "this" during the thread switch? 
    // Does the gcroot in uv_owin_async->context ensure that?
    this->task = task;
    BOOL ret = PostQueuedCompletionStatus(
        uv_default_loop()->iocp, 
        0, 
        (ULONG_PTR)NULL, 
        &this->uv_owin_async->uv_async.async_req.overlapped);
}

void ClrFuncInvokeContext::CompleteOnV8Thread()
{
    if (debugMode)
        System::Console::WriteLine("CompleteOnV8Thread");

    HandleScope handleScope;

    this->DisposeUvOwinAsync();

    if (this->callback) 
    {
        Handle<Value> argv[] = { Undefined(), Undefined() };
        int argc = 1;

        switch (this->task->Status) {
            default:
                argv[0] = v8::String::New("The operation reported completion in an unexpected state.");
            break;
            case TaskStatus::Faulted:
                if (this->task->Exception != nullptr) {
                    argv[0] = exceptionCLR2stringV8(this->task->Exception);
                }
                else {
                    argv[0] = v8::String::New("The operation has failed with an undetermined error.");
                }
            break;
            case TaskStatus::Canceled:
                argv[0] = v8::String::New("The operation was cancelled.");
            break;
            case TaskStatus::RanToCompletion:
                argc = 2;
                TryCatch try_catch;
                argv[1] = ClrFunc::MarshalCLRToV8(this->task->Result);
                if (try_catch.HasCaught()) 
                {
                    argc = 1;
                    argv[0] = try_catch.Exception();
                }
            break;
        };

        TryCatch try_catch;
        (*(this->callback))->Call(v8::Context::GetCurrent()->Global(), argc, argv);
        this->DisposeCallback();
        if (try_catch.HasCaught()) 
        {
            node::FatalException(try_catch);
        }        
    }
}
