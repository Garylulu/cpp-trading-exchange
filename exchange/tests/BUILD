load("@rules_cc//cc:defs.bzl", "cc_test")

# The same tests run against both order books.

cc_test(
    name = "market_data_test",
    srcs = ["market_data_test.cpp"],
    copts = [
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wpedantic",
    ],
    linkopts = ["-pthread"],
    deps = [
        "//common:locked_queue",
        "//common:order",
        "//common:order_book",
        "//exchange:market_data",
        "//exchange:matching_engine",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "market_data_test_ta",
    srcs = ["market_data_test.cpp"],
    copts = [
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wpedantic",
    ],
    linkopts = ["-pthread"],
    local_defines = ["USE_TICK_ARRAY_BOOK"],
    deps = [
        "//common:locked_queue",
        "//common:order",
        "//common:order_book_ta",
        "//exchange:market_data",
        "//exchange:matching_engine",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
