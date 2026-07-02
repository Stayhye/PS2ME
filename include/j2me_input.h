/*
 * input.h - camada de entrada do HAL (semente do mapeamento PS2 -> keypad J2ME)
 *
 * No MIDP, o Canvas recebe teclas: setas de navegacao (UP/DOWN/LEFT/RIGHT),
 * FIRE, e teclas 0-9/softkeys. Aqui traduzimos o controle do PS2 para esse
 * modelo. Por ora expomos so o essencial para o M1 (navegacao + fire).
 */
#ifndef J2ME_INPUT_H
#define J2ME_INPUT_H

#include <tamtypes.h>

typedef struct {
    u8  up, down, left, right;  /* D-pad -> navegacao do Canvas */
    u8  fire;                   /* Cross -> GAME_A / FIRE        */
    u8  soft_a, soft_b;         /* L1 / R1 -> softkeys           */
    u32 raw;                    /* bits crus do pad (ativo-alto) */
} j2me_keys_t;

/* Carrega modulos IOP (SIO2MAN/PADMAN) e abre o pad. Retorna 1 se ok. */
int input_init(void);

/* Le o estado atual do pad para *out. Retorna 1 se ok, 0 se sem pad. */
int input_poll(j2me_keys_t *out);

#endif /* J2ME_INPUT_H */
