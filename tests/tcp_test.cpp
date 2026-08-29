#include <gtest/gtest.h>

import std;
import asio;
import mcpplibs.cmp;

namespace {

using mcpplibs::cmp::IoContext;

static_assert(std::default_initializable<IoContext>);
static_assert(std::destructible<IoContext>);
static_assert(std::is_final_v<IoContext>);
static_assert(!std::copy_constructible<IoContext>);
static_assert(!std::move_constructible<IoContext>);
static_assert(!std::is_copy_assignable_v<IoContext>);
static_assert(!std::is_move_assignable_v<IoContext>);

TEST(CmpTcpTest, IoContextStartsAndStopsCleanly) {
    IoContext context {};
}

}  // namespace
