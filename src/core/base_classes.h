//
// Created by Kotarou on 2022/5/19.
//

#ifndef YADDNSC_CORE_BASE_CLASSES_H
#define YADDNSC_CORE_BASE_CLASSES_H

class NonCopyable {
public:
    NonCopyable() = default;

    ~NonCopyable() = default;

    // Copy construct
    NonCopyable(NonCopyable const &) = delete;

    // Copy assign
    NonCopyable &operator=(NonCopyable const &) = delete;

    // Move construct
    NonCopyable(NonCopyable &&) = default;

    // Move assign
    NonCopyable &operator=(NonCopyable &&) = default;
};

class NonMovable {
public:
    NonMovable() = default;

    ~NonMovable() = default;

    // Move construct
    NonMovable(NonMovable &&) = delete;

    // Move assign
    NonMovable &operator=(NonMovable &&) = delete;

    // Copy construct
    NonMovable(NonMovable const &) = default;

    // Copy assign
    NonMovable &operator=(NonMovable const &) = default;
};

class RestrictedClass {
public:
    RestrictedClass() = default;

    ~RestrictedClass() = default;

    // Copy construct
    RestrictedClass(RestrictedClass const &) = delete;

    // Copy assign
    RestrictedClass &operator=(RestrictedClass const &) = delete;

    // Move construct
    RestrictedClass(RestrictedClass &&) = delete;

    // Move assign
    RestrictedClass &operator=(RestrictedClass &&) = delete;
};

#endif //YADDNSC_CORE_BASE_CLASSES_H
