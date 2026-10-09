#include "jk_demux.h"
#include "jk_req.h"

jk_demux_action_t jk_demux(const uint8_t *buf, size_t n, bool stalled,
                           jk_demux_out_t *out)
{
    if (out) {
        const jk_demux_out_t zero = { 0 };
        *out = zero;
    }
    if (n == 0) {
        return JK_DEMUX_NEED_MORE;
    }

    jk55_check_t ck = JK55_CK_NONE;
    const int jl = jk55_frame_len(buf, n, &ck);
    if (jl > 0) {
        if (out) {
            out->len = (size_t)jl;
            out->ck  = ck;
        }
        return JK_DEMUX_JK55;
    }
    // jl == 0 means the 55AA magic matched as far as we can see and the body is
    // still arriving. Never offer those bytes to the request recogniser: a 55AA
    // prefix could match one by coincidence, and consuming them would destroy
    // the real frame. Wait -- unless the body stopped coming.
    if (jl == 0) {
        return stalled ? JK_DEMUX_DROP1 : JK_DEMUX_NEED_MORE;
    }

    uint16_t reg = 0;
    uint8_t  pack = 0;
    const int rl = jk_req_len(buf, n, &reg, &pack);
    if (rl > 0) {
        if (out) {
            out->len      = (size_t)rl;
            out->req_reg  = reg;
            out->req_pack = pack;
        }
        return JK_DEMUX_REQ;
    }
    if (rl == 0 && !stalled) {
        return JK_DEMUX_NEED_MORE;
    }
    return JK_DEMUX_DROP1;
}
