//
// Created by Kotarou on 2026/6/27.
//

#ifndef YADDNSC_YADDNSC_EXPORT_H
#define YADDNSC_YADDNSC_EXPORT_H

/// YADDNSC_EXPORT — cross-platform symbol visibility helper.
///
/// Use this on any function/variable that must be resolved from a shared
/// library or driver module loaded at runtime (dlopen / LoadLibrary).
/// The main executable is built with -fvisibility=hidden so everything else
/// is internal by default.

#ifdef _WIN32
#define YADDNSC_EXPORT __declspec(dllexport)
#else
#define YADDNSC_EXPORT __attribute__((visibility("default")))
#endif

#endif //YADDNSC_YADDNSC_EXPORT_H
