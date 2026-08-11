#ifndef __GAME_DATA_H
#define __GAME_DATA_H

// Declarations only; definitions in user/libc/game_data.c (built into ULIB).
// 1bpp SSD1306 page layout. Array sizes:
// GROUND: 384 bytes
// CLOUD: 28 bytes
// DINO_Left: 32 bytes
// DINO_right: 32 bytes
// DINO_JUMP: 384 bytes
// CACTUS_1: 16 bytes
// CACTUS_2: 32 bytes
// CACTUS_3: 48 bytes
// CACTUS_4: 48 bytes
// COVER: 1024 bytes
// RestartDino: 195 bytes
// GameOver: 255 bytes
// Fonts (OLED_F8x16, OLED_F6x8) live in user/include/oled_font.h.

extern const unsigned char OLED_F8x16[][16];
extern const unsigned char OLED_F6x8[][6];

extern const unsigned char GROUND[];
extern const unsigned char CLOUD[];
extern const unsigned char DINO_Left[];
extern const unsigned char DINO_right[];
extern const unsigned char DINO_JUMP[];
extern const unsigned char CACTUS_1[];
extern const unsigned char CACTUS_2[];
extern const unsigned char CACTUS_3[];
extern const unsigned char CACTUS_4[];
extern const unsigned char COVER[];
extern const unsigned char RestartDino[];
extern const unsigned char GameOver[];

#endif /* __GAME_DATA_H */
