#ifndef __LOG_H
#define __LOG_H

#include "printf.h"

#define LOG_LEVEL_NONE   0
#define LOG_LEVEL_ERROR  1
#define LOG_LEVEL_WARN   2
#define LOG_LEVEL_INFO   3
#define LOG_LEVEL_DEBUG  4

#ifndef LOG_LEVEL
#define LOG_LEVEL LOG_LEVEL_INFO
#endif

#if LOG_LEVEL >= LOG_LEVEL_ERROR
#define LOG_E(fmt, ...) printf("[E] " fmt, ##__VA_ARGS__)
#else
#define LOG_E(fmt, ...) do {} while(0)
#endif

#if LOG_LEVEL >= LOG_LEVEL_WARN
#define LOG_W(fmt, ...) printf("[W] " fmt, ##__VA_ARGS__)
#else
#define LOG_W(fmt, ...) do {} while(0)
#endif

#if LOG_LEVEL >= LOG_LEVEL_INFO
#define LOG_I(fmt, ...) printf("[I] " fmt, ##__VA_ARGS__)
#else
#define LOG_I(fmt, ...) do {} while(0)
#endif

#if LOG_LEVEL >= LOG_LEVEL_DEBUG
#define LOG_D(fmt, ...) printf("[D] " fmt, ##__VA_ARGS__)
#else
#define LOG_D(fmt, ...) do {} while(0)
#endif

#endif
