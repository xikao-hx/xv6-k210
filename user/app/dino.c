// Step 3+4: DinoGame — pure-integer port of game/DinoGame.c (FreeRTOS ref).
//
//   dino
//
// Platform swaps vs the reference:
//   acos() angle   -> integer tilt threshold on AccX   (see sample_tilt)
//   rand()         -> LCG
//   nanosleep()    -> sleep(ticks)        (K210: 5 ms/tick)
//   pthread sampler-> forked child sharing the tilt value via a MAP_SHARED page
//   /dev/mpu6050   -> user/libc/mpu6050.c library
//
// Process layout:
//   child  : opens /dev/mpu6050, samples AccX every 10 ms -> *tilt_state.
//   parent : game loop reads *tilt_state for jump/restart.  On Ctrl+C the
//            parent's SIGINT handler clears the screen, prints a goodbye and
//            exits; the child is in the same pgroup and dies from its default
//            SIGINT action, so no quit flag is needed.
//
// Pure integer: objdump must show no FPU instructions (K210 FPU context is
// not saved/restored by xv6).
//
// Controls (user-confirmed): tilt forward to jump, tilt forward on GAME OVER
// to restart, Ctrl+C to quit.

#include "types.h"
#include "user.h"
#include "fcntl.h"
#include "oledfb.h"
#include "oled.h"
#include "game_data.h"
#include "mpu6050.h"

/* keys: only UP (tilt) / NONE are produced in this port */
#define KEY_UP    0x40
#define KEY_NONE  0x00

/* game globals, names kept from the reference for easy diffing */
static unsigned char DinoGame_key_Num = 0;
static unsigned char DinoGame_Last_key_Num = 0;
static unsigned char Score = 1;
static unsigned char Highest_Score = 0;
static unsigned char Game_Speed = 30;
static unsigned char failed = 0;
static unsigned char cnt = 0;

static unsigned char Speed = 4;              /* ground / cactus speed */

static unsigned int  Ground_Length = 384;
static unsigned int  Ground_Flag = 0;

static int           Cactus_Pos = 128;
static int           Cactus_Length = 8;
static unsigned char Cactus_Num = 0;

static int           Cloud_Pos = 128;
static char          Cloud_Speed = 1;
static unsigned char Cloud_Length = 28;
static char          Cloud_Height = 0;

static unsigned char Dino_Cnt = 0;

static char Jump_Speed_Arr[] = { 1, 1, 3, 3, 4, 4, 5, 6, 7 };
static int  Speed_Idx = 9 - 1;
static int  height = 0;
static char Jump_State = 0;

/* one shared int: child writes tilt, parent reads it.  Single writer /
   single reader on a word, deliberately lock-free. */
volatile int *tilt_state;

/* sampler-child I2C fd */
static int  mpu_fd = -1;

/* ---- LCG: rand() replacement ---- */
static unsigned int lcg_state = 1;

static unsigned int
lcg_rand(void)
{
  lcg_state = lcg_state * 1103515245u + 12345u;
  return (lcg_state >> 16) & 0x7FFFu;
}

static void
sample_tilt(void)
{
  int t;

  if (mpu_fd < 0)
    return;
  t = mpu6050_tilt(mpu_fd);
  printf("tilt: %d", t);
  if (t < 0)
    return;
  *tilt_state = t;
}

static void
parameter_reset(void)
{
  DinoGame_key_Num = 0;
  DinoGame_Last_key_Num = 0;

  Score = 1;
  Game_Speed = 30;
  failed = 0;
  cnt = 0;

  Speed = 4;

  Ground_Length = 384;
  Ground_Flag = 0;

  Cactus_Pos = 128;
  Cactus_Length = 8;
  Cactus_Num = 0;

  Cloud_Pos = 128;
  Cloud_Speed = 1;
  Cloud_Length = 28;
  Cloud_Height = 0;

  Dino_Cnt = 0;

  Speed_Idx = 9 - 1;
  height = 0;
  Jump_State = 0;
}

/* Ctrl+C: parent clears the screen, shows a goodbye on the OLED, exits.
   The child in the same pgroup dies from its default SIGINT action -- it
   only samples I2C, so nothing to clean up there. */
static void
on_sigint(int sig)
{
  (void)sig;
  OLED_Clear();
  OLED_ShowString(8, 0, "dino: manually", OLED_8X16);
  OLED_ShowString(12, 16, "terminated by", OLED_8X16);
  OLED_ShowString(40, 32, "Ctrl+C", OLED_8X16);
  OLED_Flush();
  exit(0);
}

int
main(void)
{
  int pid;

  OLED_init();                     /* opens+mmaps fb; exits on failure */
  OLED_Clear();

  /* one shared page (4096 == PGSIZE), then fork the sampler so the game
     never blocks on I2C itself */
  tilt_state = mmap(0, 4096, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (tilt_state == (void *)-1 || tilt_state == 0) {
    printf("dino: sampler mmap failed\n");
    exit(1);
  }
  *tilt_state = 0;

  pid = fork();
  if (pid < 0) {
    printf("dino: fork failed\n");
    exit(1);
  }

  if (pid == 0) {
    /* ---- sampler child: independent tilt producer ---- */
    mpu_fd = mpu6050_open();
    if (mpu_fd < 0) {
      printf("dino: sampler: /dev/mpu6050 open failed, no tilt input\n");
      exit(1);
    }
    /* Soft reset wipes leftover config (e.g. ±16g) from earlier mpu6050 /
       i2ctest runs that would otherwise survive until power-off and corrupt
       these reads (observed on the real board). */
    if (mpu6050_reset(mpu_fd) < 0) {
      printf("dino: sampler: MPU6050 reset failed\n");
      exit(1);
    }
    for (;;) {
      sample_tilt();
      sleep(2);                        /* 10 ms sample period */
    }
  }

  /* ---- game parent ---- */
  if (signal(SIGINT, on_sigint) == SIG_ERR)
    printf("dino: warning: SIGINT handler not set\n");

  parameter_reset();

  /* cover screen; reference parked here waiting for a key, we just show it */
  OLED_Clear();
  OLED_ShowImage(0, 0, 128, 64, COVER);
  OLED_Flush();
  sleep(40);                           /* 200 ms so the cover is visible */

  for (;;) {
    OLED_Clear();

    /* ---- game over screen ---- */
    if (failed == 1) {
      OLED_ShowImage(0, 24, 39, 40, RestartDino);
      OLED_ShowString(0, 1, "GAME OVER!", OLED_8X16);
      OLED_ShowString(45, 24, "High Score:", OLED_6X8);
      OLED_ShowNum(98, 32, Highest_Score, 5, OLED_6X8);
      OLED_ShowString(45, 45, "Now Score:", OLED_6X8);
      OLED_ShowNum(98, 53, Score, 5, OLED_6X8);
      OLED_Flush();

      /* tilt to restart: tight 10 ms poll so a short gesture is not lost,
         then wait for release so the fresh game starts flat and clean. */
      while (!*tilt_state)
        sleep(2);
      while (*tilt_state)
        sleep(1);

      if (Score > Highest_Score)
        Highest_Score = Score;
      parameter_reset();
      continue;
    }

    cnt++;
    if (cnt == 6) {
      Score++;
      cnt = 0;
    }

    /* read the sampler's latest tilt only while on the ground */
    if (height <= 0) {
      DinoGame_Last_key_Num = DinoGame_key_Num;
      DinoGame_key_Num = *tilt_state ? KEY_UP : KEY_NONE;
    }

    /* ---- ground ---- */
    if (Ground_Flag < (Ground_Length - 128 - 4)) {
      OLED_ShowImage(0, 56, 128, 8, &GROUND[Ground_Flag]);
      Ground_Flag += Speed;
    } else {
      OLED_ShowImage(0, 56, 128, 8, &GROUND[Ground_Flag]);
      Ground_Flag = 0;
    }

    /* ---- cloud ---- */
    if (Cloud_Pos + Cloud_Length <= 0)
      Cloud_Pos = 128;
    if (Cloud_Pos == 128)
      Cloud_Height = lcg_rand() % 16;

    OLED_ShowString(0, 0, "                ", OLED_8X16);  /* clear top */
    OLED_ShowString(0, 8, "                ", OLED_8X16);
    if (Cloud_Pos < 0)
      OLED_ShowImage(0, Cloud_Height, Cloud_Length + Cloud_Pos, 8,
                     &CLOUD[-Cloud_Pos]);
    else
      OLED_ShowImage(Cloud_Pos, Cloud_Height, Cloud_Length, 8, CLOUD);
    Cloud_Pos = Cloud_Pos - Cloud_Speed;

    /* ---- dino: jump (two-phase) or run (two-frame) ---- */
    if ((height > 0 || DinoGame_key_Num == KEY_UP) &&
        (DinoGame_Last_key_Num != DinoGame_key_Num)) {
      if (Jump_State == 0) {           /* rising */
        height += Jump_Speed_Arr[Speed_Idx];
        Speed_Idx--;
        OLED_ShowImage(16, 64 - 20 - height, 16, 16, &DINO_JUMP[7 * 48]);
        if (Speed_Idx <= 0)
          Speed_Idx = 0;
      }
      if (Jump_State == 1) {           /* falling */
        height -= Jump_Speed_Arr[Speed_Idx];
        Speed_Idx++;
        OLED_ShowImage(16, 64 - 20 - height, 16, 16, &DINO_JUMP[7 * 48]);
        if (Speed_Idx >= 9 - 1)
          Speed_Idx = 9 - 1;
      }
      if (height >= 31) {
        Jump_State = 1;
        height = 31;
      }
      if (height <= 0) {
        Jump_State = 0;
        height = 0;
      }
    } else {
      Dino_Cnt++;
      Dino_Cnt = Dino_Cnt % 2;
      if (Dino_Cnt == 0)
        OLED_ShowImage(16, 48, 16, 16, DINO_Left);
      else
        OLED_ShowImage(16, 48, 16, 16, DINO_right);
    }

    /* ---- cactus ---- */
    if (Cactus_Pos == 128)
      Cactus_Num = lcg_rand() % 4;
    if (Cactus_Num == 0)
      Cactus_Length = 8;
    else if (Cactus_Num == 1)
      Cactus_Length = 16;
    else if (Cactus_Num == 2 || Cactus_Num == 3)
      Cactus_Length = 24;

    if (Cactus_Pos < 0) {
      if (Cactus_Num == 0)
        OLED_ShowImage(0, 48, Cactus_Pos + Cactus_Length, 16,
                       &CACTUS_1[-Cactus_Pos]);
      else if (Cactus_Num == 1)
        OLED_ShowImage(0, 48, Cactus_Pos + Cactus_Length, 16,
                       &CACTUS_2[-Cactus_Pos]);
      else if (Cactus_Num == 2)
        OLED_ShowImage(0, 48, Cactus_Pos + Cactus_Length, 16,
                       &CACTUS_3[-Cactus_Pos]);
      else
        OLED_ShowImage(0, 48, Cactus_Pos + Cactus_Length, 16,
                       &CACTUS_4[-Cactus_Pos]);
    } else {
      if (Cactus_Num == 0)
        OLED_ShowImage(Cactus_Pos, 48, Cactus_Length, 16, CACTUS_1);
      else if (Cactus_Num == 1)
        OLED_ShowImage(Cactus_Pos, 48, Cactus_Length, 16, CACTUS_2);
      else if (Cactus_Num == 2)
        OLED_ShowImage(Cactus_Pos, 48, Cactus_Length, 16, CACTUS_3);
      else
        OLED_ShowImage(Cactus_Pos, 48, Cactus_Length, 16, CACTUS_4);
    }
    Cactus_Pos = Cactus_Pos - Speed;
    if (Cactus_Pos + Cactus_Length <= 0)
      Cactus_Pos = 128;

    /* ---- collision ---- */
    if (height < 16 &&
        ((Cactus_Pos > 16 && Cactus_Pos < 32) ||
         (Cactus_Pos + Cactus_Length > 17 &&
          Cactus_Pos + Cactus_Length < 31)))
      failed = 1;

    /* ---- score ---- */
    OLED_ShowString(35, 0, "HI:", OLED_6X8);
    OLED_ShowNum(58, 0, Highest_Score, 5, OLED_6X8);
    OLED_ShowNum(98, 0, Score, 5, OLED_6X8);

    Game_Speed = Score / 20;
    if (Game_Speed > 29)
      Game_Speed = 29;

    OLED_Flush();

    /* frame pacing: (30-Game_Speed)/12 ticks ≈ 10 ms early, ~5 ms late.
       The full-screen FLUSH (~24 ms) is the real per-frame cost; a smaller
       sleep gains nothing until Step 5 does dirty-page refresh. */
    {
      int ticks = (30 - Game_Speed) / 12;
      if (ticks < 1)
        ticks = 1;
      sleep(ticks);
    }
  }
}
