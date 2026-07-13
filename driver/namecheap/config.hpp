//
// Created by Kotarou on 2026/7/13.
//

#ifndef YADDNSC_DRV_NAMECHEAP_CONFIG_HPP
#define YADDNSC_DRV_NAMECHEAP_CONFIG_HPP

#include <string>

#include <glaze/glaze.hpp>

/// Namecheap Dynamic DNS driver configuration parameters.
struct NamecheapParams {
    /// DDNS password set in Namecheap's control panel
    /// (Advanced DNS tab -> Dynamic DNS section -> Dynamic DNS Password).
    std::string password;
};

template<>
struct glz::meta<NamecheapParams> {
    using T = NamecheapParams;
    static constexpr auto value = object("password", &T::password);
};

#endif  // YADDNSC_DRV_NAMECHEAP_CONFIG_HPP
