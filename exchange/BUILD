load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

cc_library(
    name = "market_data",
    hdrs = [
        "market_data/market_data.h",
        "market_data/market_data_client.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//common:locked_queue",
        "//common:order",
    ],
)

cc_library(
    name = "matching_engine",
    hdrs = ["matcher/matching_engine.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":market_data",
        "//common:locked_queue",
        "//common:order",
    ],
)

cc_binary(
    name = "exchange_main",
    srcs = ["exchange_main.cpp"],
    copts = [
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wpedantic",
    ],
    linkopts = ["-pthread"],
    deps = [
        ":market_data",
        ":matching_engine",
        "//common:locked_queue",
        "//common:order",
        "//common:order_book_ta",
    ],
)
