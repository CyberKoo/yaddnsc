#include <string>

#include <yaddnsc/sdk/signing.hpp>

std::string consumer_hash() {
    return yaddnsc::sdk::signing::sha256_hex("consumer");
}
