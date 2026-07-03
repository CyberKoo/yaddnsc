//
// Created by Kotarou on 2022/5/19.
//

#ifndef YADDNSC_CORE_MIXIN_H
#define YADDNSC_CORE_MIXIN_H

// Tag types that disable copy/move semantics via [[no_unique_address]] members.
// Zero overhead — the tags carry no data, [[no_unique_address]] elides them.

struct NoCopy {
    NoCopy() = default;

    NoCopy(const NoCopy &) = delete;

    NoCopy &operator=(const NoCopy &) = delete;

    NoCopy(NoCopy &&) = default;

    NoCopy &operator=(NoCopy &&) = default;
};

struct NoMove {
    NoMove() = default;

    NoMove(NoMove &&) = delete;

    NoMove &operator=(NoMove &&) = delete;

    NoMove(const NoMove &) = default;

    NoMove &operator=(const NoMove &) = default;
};

#endif //YADDNSC_CORE_MIXIN_H
