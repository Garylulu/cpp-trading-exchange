load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "perf_utils",
    hdrs = ["perf_utils.h"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "time_utils",
    hdrs = ["time_utils.h"],
    visibility = ["//visibility:public"],
    deps = [":perf_utils"],
)

cc_library(
    name = "order",
    hdrs = ["order.h"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "mem_pool_o",
    hdrs = ["mem_pool_o.h"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "order_book",
    hdrs = ["order_book.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":mem_pool_o",
        ":order",
    ],
)
