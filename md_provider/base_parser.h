#pragma once

// Parser: the base class a venue parser derives from - a reused
// simdjson::ondemand::parser and a growable input buffer, so a steady message
// stream does no per-message allocation. One instance per thread; not
// thread-safe.

#include <simdjson.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace screener {

// Shared state for a venue parser: a reusable simdjson parser and input
// buffer, so a steady stream of messages does no per-message allocation.
//
// NOT thread-safe: one instance serves one thread. In the MVP that is the
// only thread there is.
class Parser {
   protected:
    // Copies `raw` into input_ (grown to raw.size() + SIMDJSON_PADDING) and
    // returns a view simdjson can iterate. input_ only ever grows, so once it
    // has seen the largest message a steady stream does no allocation.
    simdjson::padded_string_view Load(std::string_view raw) {
        input_.resize(raw.size() + simdjson::SIMDJSON_PADDING);
        std::memcpy(input_.data(), raw.data(), raw.size());
        return simdjson::padded_string_view(input_.data(), raw.size(), input_.size());
    }

    simdjson::ondemand::parser parser_;
    std::string input_;
};

}  // namespace screener
