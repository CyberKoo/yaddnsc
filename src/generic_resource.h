//
// Created by Kotarou on 2022/4/20.
//

#ifndef YADDNSC_GENERIC_RESOURCE_H
#define YADDNSC_GENERIC_RESOURCE_H

#include <functional>

template<typename T>
class GeneralResource {
public:
    GeneralResource(std::function<T()>, std::function<void(T)>) :
            constructor(std::move(constructor)),
            destructor(std::move(destructor)) {
        resource = constructor();
    }

    ~GeneralResource() {
        destructor(resource);
    }

    GeneralResource(GeneralResource &&) noexcept = default;

    GeneralResource(GeneralResource const &) = delete;

    GeneralResource &operator=(const GeneralResource &) = delete;

    T &get() {
        return resource;
    }

    T &get() const {
        return resource;
    }

private:
    T resource;
    std::function<T()> constructor;
    std::function<void(T)> destructor;
};

#endif //YADDNSC_GENERIC_RESOURCE_H
