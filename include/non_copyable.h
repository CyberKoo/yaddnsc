//
// Created by Kotarou on 2022/4/6.
//

#ifndef YADDNSC_NON_COPYABLE_H
#define YADDNSC_NON_COPYABLE_H

class NonCopyable {
public:
    // delete copy and move constructors and assign operators
    // Copy construct
    NonCopyable(NonCopyable const &) = delete;

    // Move construct
    NonCopyable(NonCopyable &&) = delete;

    // Copy assign
    NonCopyable &operator=(NonCopyable const &) = delete;

    // Move assign
    NonCopyable &operator=(NonCopyable &&) = delete;

    // Default construct
    NonCopyable() = default;
};

#endif //YADDNSC_NON_COPYABLE_H
