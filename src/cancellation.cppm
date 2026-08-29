export module mcpplibs.cmp:cancellation;

import std;

export namespace mcpplibs::cmp {

class OperationCancelled final : public std::exception {
public:
    [[nodiscard]] const char* what() const noexcept override {
        return "operation cancelled";
    }
};

}  // namespace mcpplibs::cmp
