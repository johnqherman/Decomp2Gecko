// exceptions = the user has to act, or corrupt input; ordinary "didn't apply" outcomes are return values
#pragma once

#include <stdexcept>

namespace Decomp2Gecko {

class ToolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

#define DECOMP2GECKO_ERROR(ClassName) \
    class ClassName : public ToolError { \
    public: \
        using ToolError::ToolError; \
    };

DECOMP2GECKO_ERROR(InvalidValueError)
DECOMP2GECKO_ERROR(MissingFileError)
DECOMP2GECKO_ERROR(NotBuiltError)
DECOMP2GECKO_ERROR(InternalError)

// the layout can't place the mod; message names the chunk
DECOMP2GECKO_ERROR(LayoutError)

#undef DECOMP2GECKO_ERROR

} // namespace Decomp2Gecko
