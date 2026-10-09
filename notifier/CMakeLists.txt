# notifier/CMakeLists.txt
#
# Telegram notifications. Depends on md_provider for AsyncHttpsGet only.
#
# That dependency is a WART: a chat notifier has no business linking a market
# data provider. The clean fix is to lift async_rest.{h,cpp} into its own
# `net` library that md_provider and notifier both use - deliberately not done
# in the MVP, because it is churn across four CMakeLists for no behaviour
# change. Written down here rather than discovered later.
add_library(notifier STATIC
    telegram_notifier.cpp
)

target_include_directories(notifier PUBLIC
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(notifier PUBLIC
    types
    config
    logger
    md_provider
)
