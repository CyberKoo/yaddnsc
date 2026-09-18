# Examples

## Memory Management

```cpp
// BAD: Raw memory management
int* calculateSum(int* a, int* b) {
    int* result = new int(0);
    *result = *a + *b;
    return result;
}

// GOOD: Value semantics
[[nodiscard]] int calculateSum(int a, int b) const {
    return a + b;
}

// BAD: Manual buffer
class Buffer {
    char* data_;
    size_t size_;
public:
    Buffer(size_t s) : data_(new char[s]), size_(s) {}
    ~Buffer() { delete[] data_; }
};

// GOOD: RAII with standard containers
class Buffer {
    std::vector<char> data_;
public:
    explicit Buffer(size_t size) : data_(size) {}
};
```

## Error Handling

```cpp
// BAD: C-style
int divide(int a, int b, int* result) {
    if (b == 0) return -1;
    *result = a / b;
    return 0;
}

// GOOD: std::expected (C++23) — no allocation, safe to mark noexcept
enum class MathError { DIVISION_BY_ZERO };
[[nodiscard]] std::expected<int, MathError> divide(int a, int b) noexcept {
    if (b == 0) return std::unexpected(MathError::DIVISION_BY_ZERO);
    return a / b;
}

// BAD: Function design — throws for retryable I/O errors
// The root problem is that send() and read() throw for errors the
// caller should handle (timeout, transient failure). Fix the functions,
// not the call site.
void send(const Packet&);     // throws on timeout — should return expected
void read(Response&);         // throws on checksum error — should return expected

try {
    send(pkt);
    read(rsp);
} catch (const DnsLookupException& e) {
    return std::unexpected(e);
}
// ✗ Wrong: The functions themselves are incorrectly designed.
//   The try-catch papers over the design problem.

// GOOD: Functions return expected — no throw/catch needed
[[nodiscard]] std::expected<void, SendError> send(const Packet&) noexcept;
[[nodiscard]] std::expected<Response, ReadError> read() noexcept;

auto s = send(pkt);
if (!s) return std::unexpected(s.error());

auto rsp = read();
if (!rsp) return std::unexpected(std::move(rsp.error()));

return std::move(*rsp);

// GOOD: catch only logs at termination point — no rethrow
void handle_request() {
    try {
        process();  // throws on precondition failure — cannot proceed
    } catch (const std::exception& e) {
        LOG_ERROR("Fatal: {}", e.what());
    }
}

// GOOD: catch logs and propagates — not the final termination point
void inner() {
    try {
        process();
    } catch (const std::exception& e) {
        LOG_WARN("Inner failed: {}", e.what());
        throw;
    }
}

// GOOD: Strong exception safety with rollback
void transfer(Account& from, Account& to, Money amount) {
    auto snapshot_from = from;
    auto snapshot_to = to;
    from.withdraw(amount);
    try {
        to.deposit(amount);
    } catch (...) {
        from = snapshot_from;   // rollback, nothrow
        to = snapshot_to;       // rollback, nothrow
        throw;
    }
}

// GOOD: Error translation at module boundary — multiple error categories
[[nodiscard]] std::expected<Response, ResolveError> resolve(const Query& q) {
    try {
        return do_resolve(q);
    } catch (const TimeoutError& e) {
        return std::unexpected(ResolveError::TIMEOUT);
    } catch (const FormatError& e) {
        return std::unexpected(ResolveError::INVALID_RESPONSE);
    } catch (const NetworkError& e) {
        return std::unexpected(ResolveError::NETWORK_FAILURE);
    }
}
```

## Code Reuse

```cpp
// BAD: Reimplementing string splitting
std::vector<std::string> split(const std::string& str, char delim) { /* ... */ }

// GOOD: Using project infrastructure
#include "project/strings/string_utils.h"
void process_items(std::string_view input) {
    auto tokens = strings::split(input, ',');
    for (auto token : tokens) process(strings::trim(token));
}

// BAD: Ad-hoc logging
void handle_request(const Request& req) {
    std::cout << "Processing: " << req.id() << std::endl;
}

// GOOD: Project logging facade
void handle_request(const Request& req) {
    LOG_INFO("Processing request: {}", req.id());
}
```

## Move Semantics & Lifetime

```cpp
// BAD: Use-after-move
auto other = std::move(data);
std::cout << data.size();

// GOOD: Reassign before reuse
auto other = std::move(data);
data = {4, 5, 6};
std::cout << data.size();

// BAD: Dangling reference (-Wreturn-stack-address)
const std::string& get_name() {
    std::string local = "temp";
    return local;
}

// GOOD: Return by value (NRVO applies)
std::string get_name() {
    std::string local = "temp";
    return local;
}
```

## String Safety

```cpp
// BAD: Unsafe string_view with C functions
void call_open(std::string_view filename) {
    FILE* f = std::fopen(filename.data(), "r");  // May not be null-terminated
}

// GOOD: Explicit conversion
void call_open(std::string_view filename) {
    std::string str(filename);
    FILE* f = std::fopen(str.c_str(), "r");
}

// GOOD: API signals null-termination requirement
void call_open(const std::string& filename) {
    FILE* f = std::fopen(filename.c_str(), "r");
}

// GOOD: Caching for repeated use
class ConfigParser {
    std::string config_path_;
public:
    explicit ConfigParser(std::string_view path) : config_path_(path) {}
    void load() {
        FILE* f = std::fopen(config_path_.c_str(), "r");
        // ... multiple operations using config_path_.c_str()
        std::fclose(f);
    }
};
```
