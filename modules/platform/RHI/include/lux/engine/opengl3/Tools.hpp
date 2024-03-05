#pragma once
#include <thread>
#include <mutex>

namespace lux::gapi::opengl
{   
    template<class T> concept bindable_obj = requires(T obj){
        obj.bind();
        obj.endBind();
    };

    template<bindable_obj T>
    class scope_bind
    {
    public:
        using obj_type = T;

        explicit scope_bind(T& ref)
            : _obj_ref(ref)
        {
            _obj_ref.bind();
        }

        scope_bind(const scope_bind&) = delete;
        scope_bind& operator=(const scope_bind&) = delete;

        ~scope_bind()
        {
            _obj_ref.endBind();
        }

    private:
        T& _obj_ref;
    };
} // namespace lux::gapi::opengl
