/*
 * input.c - implementacao da camada de entrada (libpad).
 * Modelado em ee/rpc/pad/samples/pad/pad.c, reduzido ao necessario para
 * um controle digital (setas + botoes), sem dualshock/atuadores.
 */
#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <stdio.h>

#include "libpad.h"
#include "j2me_input.h"

/* buffer de estado do pad, fornecido pelo usuario (1 por pad), alinhado 64 */
static char pad_buf[256] __attribute__((aligned(64)));

#define PORT 0   /* conector 1 */
#define SLOT 0   /* 0 quando sem multitap */

static void load_modules(void)
{
    int ret;
    ret = SifLoadModule("rom0:SIO2MAN", 0, NULL);
    if (ret < 0) printf("[input] falha ao carregar SIO2MAN: %d\n", ret);
    ret = SifLoadModule("rom0:PADMAN", 0, NULL);
    if (ret < 0) printf("[input] falha ao carregar PADMAN: %d\n", ret);
}

/* Espera o pad sair do estado transitorio. Retorna 0 se desconectado. */
static int wait_ready(void)
{
    int state, guard = 0;
    do {
        state = padGetState(PORT, SLOT);
        if (state == PAD_STATE_DISCONN) return 0;
        if (++guard > 2000000) return 0;   /* nao trava para sempre */
    } while (state != PAD_STATE_STABLE && state != PAD_STATE_FINDCTP1);
    return 1;
}

int input_init(void)
{
    SifInitRpc(0);
    load_modules();
    padInit(0);

    if (padPortOpen(PORT, SLOT, pad_buf) == 0) {
        printf("[input] padPortOpen falhou\n");
        return 0;
    }
    return wait_ready();
}

int input_poll(j2me_keys_t *out)
{
    struct padButtonStatus b;
    u32 d;
    int state = padGetState(PORT, SLOT);

    if (state != PAD_STATE_STABLE && state != PAD_STATE_FINDCTP1)
        return 0;
    if (padRead(PORT, SLOT, &b) == 0)
        return 0;

    d = 0xffff ^ b.btns;   /* pad e ativo-baixo; invertendo -> ativo-alto */
    out->raw    = d;
    out->up     = (d & PAD_UP)    ? 1 : 0;
    out->down   = (d & PAD_DOWN)  ? 1 : 0;
    out->left   = (d & PAD_LEFT)  ? 1 : 0;
    out->right  = (d & PAD_RIGHT) ? 1 : 0;
    out->fire   = (d & PAD_CROSS) ? 1 : 0;
    out->soft_a = (d & PAD_L1)    ? 1 : 0;
    out->soft_b = (d & PAD_R1)    ? 1 : 0;
    return 1;
}
