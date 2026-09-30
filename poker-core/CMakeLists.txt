cmake_minimum_required(VERSION 3.14)
project(PokerPlugin LANGUAGES CXX)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LogosModule.cmake")
    include(cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found")
endif()

# OpenSSL provides the big-integer modular exponentiation (BN_mod_exp) that the
# SRA mental-poker cipher needs. `openssl` is declared in metadata.json
# (nix.packages.runtime) so it lands in buildInputs; FIND_PACKAGES / LINK_LIBRARIES
# below wire find_package(OpenSSL) + link OpenSSL::Crypto into the plugin.
logos_module(
    NAME poker
    SOURCES
        src/poker_interface.h
        src/poker_crypto.h
        src/poker_crypto.cpp
        src/poker_game.h
        src/poker_game.cpp
        src/poker_plugin.h
        src/poker_plugin.cpp
    FIND_PACKAGES
        OpenSSL
    LINK_LIBRARIES
        OpenSSL::Crypto
)
