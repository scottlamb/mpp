/*
 * Copyright 2021 Rockchip Electronics Co. LTD
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define MODULE_TAG "hal_h264e_vepu580"

#include <string.h>

#include "mpp_env.h"
#include "mpp_mem.h"
#include "mpp_common.h"
#include "mpp_frame_impl.h"
#include "mpp_rc.h"

#include "h264e_sps.h"
#include "h264e_pps.h"
#include "h264e_slice.h"

#include "hal_h264e_debug.h"
#include "hal_bufs.h"
#include "mpp_enc_hal.h"
#include "vepu541_common.h"
#include "hal_h264e_vepu580_reg.h"

#define DUMP_REG 0

typedef struct HalH264eVepu580Ctx_t {
    MppEncCfgSet            *cfg;

    MppDev                  dev;
    RK_S32                  frame_cnt;

    /* buffers management */
    HalBufs                 hw_recn;
    RK_S32                  pixel_buf_fbc_hdr_size;
    RK_S32                  pixel_buf_fbc_bdy_size;
    RK_S32                  pixel_buf_size;
    RK_S32                  thumb_buf_size;
    RK_S32                  max_buf_cnt;

    /* external line buffer over 4K */
    MppBufferGroup          ext_line_buf_grp;
    MppBuffer               ext_line_buf;
    RK_S32                  ext_line_buf_size;

    /* syntax for input from enc_impl */
    RK_U32                  updated;
    H264eSps                *sps;
    H264ePps                *pps;
    H264eSlice              *slice;
    H264eFrmInfo            *frms;
    H264eReorderInfo        *reorder;
    H264eMarkingInfo        *marking;
    H264ePrefixNal          *prefix;

    /* syntax for output to enc_impl */
    EncRcTaskInfo           hal_rc_cfg;

    /* roi */
    MppEncROICfg            *roi_data;
    MppBufferGroup          roi_grp;
    MppBuffer               roi_buf;
    RK_S32                  roi_buf_size;

    /* osd */
    Vepu541OsdCfg           osd_cfg;

    /* register */
    HalVepu580RegSet        regs_set;
} HalH264eVepu580Ctx;

#define CHROMA_KLUT_TAB_SIZE    (24 * sizeof(RK_U32))

static RK_U32 h264e_klut_weight[30] = {
    0x0a000010, 0x00064000, 0x14000020, 0x000c8000,
    0x28000040, 0x00194000, 0x50800080, 0x0032c000,
    0xa1000100, 0x00658000, 0x42800200, 0x00cb0001,
    0x85000400, 0x01964002, 0x0a000800, 0x032c8005,
    0x14001000, 0x0659400a, 0x28802000, 0x0cb2c014,
    0x51004000, 0x1965c028, 0xa2808000, 0x32cbc050,
    0x4500ffff, 0x659780a1, 0x8a81fffe, 0xCC000142,
    0xFF83FFFF, 0x000001FF,
};

static RK_U32 dump_l1_reg = 0;
static RK_U32 dump_l2_reg = 0;

static RK_S32 h264_aq_tthd_default[16] = {
    0,  0,  0,  0,
    3,  3,  5,  5,
    8,  8,  8,  15,
    15, 20, 25, 25,
};

static RK_S32 h264_P_aq_step_default[16] = {
    -8, -7, -6, -5,
    -4, -3, -2, -1,
    0,  1,  2,  3,
    4,  5,  7,  8,
};

static RK_S32 h264_I_aq_step_default[16] = {
    -8, -7, -6, -5,
    -4, -3, -2, -1,
    0,  1,  3,  3,
    4,  5,  8,  8,
};

static MPP_RET hal_h264e_vepu580_deinit(void *hal)
{
    HalH264eVepu580Ctx *p = (HalH264eVepu580Ctx *)hal;

    hal_h264e_dbg_func("enter %p\n", p);

    if (p->dev) {
        mpp_dev_deinit(p->dev);
        p->dev = NULL;
    }

    if (p->roi_buf) {
        mpp_buffer_put(p->roi_buf);
        p->roi_buf = NULL;
    }

    if (p->roi_grp) {
        mpp_buffer_group_put(p->roi_grp);
        p->roi_grp = NULL;
    }

    if (p->ext_line_buf) {
        mpp_buffer_put(p->ext_line_buf);
        p->ext_line_buf = NULL;
    }

    if (p->ext_line_buf_grp) {
        mpp_buffer_group_put(p->ext_line_buf_grp);
        p->ext_line_buf_grp = NULL;
    }

    if (p->hw_recn) {
        hal_bufs_deinit(p->hw_recn);
        p->hw_recn = NULL;
    }

    hal_h264e_dbg_func("leave %p\n", p);

    return MPP_OK;
}

static MPP_RET hal_h264e_vepu580_init(void *hal, MppEncHalCfg *cfg)
{
    HalH264eVepu580Ctx *p = (HalH264eVepu580Ctx *)hal;
    MPP_RET ret = MPP_OK;

    hal_h264e_dbg_func("enter %p\n", p);

    p->cfg = cfg->cfg;

    /* update output to MppEnc */
    cfg->type = VPU_CLIENT_RKVENC;
    ret = mpp_dev_init(&cfg->dev, cfg->type);
    if (ret) {
        mpp_err_f("mpp_dev_init failed. ret: %d\n", ret);
        goto DONE;
    }
    p->dev = cfg->dev;

    ret = hal_bufs_init(&p->hw_recn);
    if (ret) {
        mpp_err_f("init vepu buffer failed ret: %d\n", ret);
        goto DONE;
    }

    p->osd_cfg.reg_base = &p->regs_set.reg_osd;
    p->osd_cfg.dev = p->dev;
    p->osd_cfg.plt_cfg = &p->cfg->plt_cfg;
    p->osd_cfg.osd_data = NULL;
    p->osd_cfg.osd_data2 = NULL;

    {   /* setup default hardware config */
        MppEncHwCfg *hw = &cfg->cfg->hw;

        hw->qp_delta_row_i  = 0;
        hw->qp_delta_row    = 1;

        memcpy(hw->aq_thrd_i, h264_aq_tthd_default, sizeof(hw->aq_thrd_i));
        memcpy(hw->aq_thrd_p, h264_aq_tthd_default, sizeof(hw->aq_thrd_p));
        memcpy(hw->aq_step_i, h264_I_aq_step_default, sizeof(hw->aq_step_i));
        memcpy(hw->aq_step_p, h264_P_aq_step_default, sizeof(hw->aq_step_p));
    }

DONE:
    if (ret)
        hal_h264e_vepu580_deinit(hal);

    hal_h264e_dbg_func("leave %p\n", p);
    return ret;
}

static void setup_hal_bufs(HalH264eVepu580Ctx *ctx)
{
    MppEncCfgSet *cfg = ctx->cfg;
    MppEncPrepCfg *prep = &cfg->prep;
    RK_S32 alignment = 64;
    RK_S32 aligned_w = MPP_ALIGN(prep->width,  alignment);
    RK_S32 aligned_h = MPP_ALIGN(prep->height, alignment);
    RK_S32 pixel_buf_fbc_hdr_size = MPP_ALIGN(aligned_w * aligned_h / 64, SZ_8K);
    RK_S32 pixel_buf_fbc_bdy_size = aligned_w * aligned_h * 3 / 2;
    RK_S32 pixel_buf_size = pixel_buf_fbc_hdr_size + pixel_buf_fbc_bdy_size;
    RK_S32 thumb_buf_size = MPP_ALIGN(aligned_w / 64 * aligned_h / 64 * 256, SZ_8K);
    RK_S32 old_max_cnt = ctx->max_buf_cnt;
    RK_S32 new_max_cnt = 2;
    MppEncRefCfg ref_cfg = cfg->ref_cfg;

    if (ref_cfg) {
        MppEncCpbInfo *info = mpp_enc_ref_cfg_get_cpb_info(ref_cfg);
        if (new_max_cnt < MPP_MAX(new_max_cnt, info->dpb_size + 1))
            new_max_cnt = MPP_MAX(new_max_cnt, info->dpb_size + 1);
    }

    if (aligned_w > SZ_4K) {
        RK_S32 ext_line_buf_size = MPP_ALIGN((aligned_w - SZ_4K) / 64 * 30 * 16, 256);

        if (NULL == ctx->ext_line_buf_grp)
            mpp_buffer_group_get_internal(&ctx->ext_line_buf_grp, MPP_BUFFER_TYPE_ION);
        else if (ext_line_buf_size != ctx->ext_line_buf_size) {
            if (ctx->ext_line_buf) {
                mpp_buffer_put(ctx->ext_line_buf);
                ctx->ext_line_buf = NULL;
            }
            mpp_buffer_group_clear(ctx->ext_line_buf_grp);
        }

        mpp_assert(ctx->ext_line_buf_grp);

        if (NULL == ctx->ext_line_buf)
            mpp_buffer_get(ctx->ext_line_buf_grp, &ctx->ext_line_buf, ext_line_buf_size);

        ctx->ext_line_buf_size = ext_line_buf_size;
    } else {
        if (ctx->ext_line_buf) {
            mpp_buffer_put(ctx->ext_line_buf);
            ctx->ext_line_buf = NULL;
        }
        if (ctx->ext_line_buf_grp) {
            mpp_buffer_group_clear(ctx->ext_line_buf_grp);
            mpp_buffer_group_put(ctx->ext_line_buf_grp);
            ctx->ext_line_buf_grp = NULL;
        }
        ctx->ext_line_buf_size = 0;
    }

    if ((ctx->pixel_buf_fbc_hdr_size != pixel_buf_fbc_hdr_size) ||
        (ctx->pixel_buf_fbc_bdy_size != pixel_buf_fbc_bdy_size) ||
        (ctx->pixel_buf_size != pixel_buf_size) ||
        (ctx->thumb_buf_size != thumb_buf_size) ||
        (new_max_cnt > old_max_cnt)) {
        size_t sizes[2];

        hal_h264e_dbg_detail("frame size %d -> %d max count %d -> %d\n",
                             ctx->pixel_buf_size, pixel_buf_size,
                             old_max_cnt, new_max_cnt);

        /* pixel buffer */
        sizes[0] = pixel_buf_size;
        /* thumb buffer */
        sizes[1] = thumb_buf_size;
        new_max_cnt = MPP_MAX(new_max_cnt, old_max_cnt);

        hal_bufs_setup(ctx->hw_recn, new_max_cnt, 2, sizes);

        ctx->pixel_buf_fbc_hdr_size = pixel_buf_fbc_hdr_size;
        ctx->pixel_buf_fbc_bdy_size = pixel_buf_fbc_bdy_size;
        ctx->pixel_buf_size = pixel_buf_size;
        ctx->thumb_buf_size = thumb_buf_size;
        ctx->max_buf_cnt = new_max_cnt;
    }
}

static MPP_RET hal_h264e_vepu580_prepare(void *hal)
{
    HalH264eVepu580Ctx *ctx = (HalH264eVepu580Ctx *)hal;
    MppEncPrepCfg *prep = &ctx->cfg->prep;

    hal_h264e_dbg_func("enter %p\n", hal);

    if (prep->change & (MPP_ENC_PREP_CFG_CHANGE_INPUT | MPP_ENC_PREP_CFG_CHANGE_FORMAT)) {
        RK_S32 i;

        // pre-alloc required buffers to reduce first frame delay
        setup_hal_bufs(ctx);
        for (i = 0; i < ctx->max_buf_cnt; i++)
            hal_bufs_get_buf(ctx->hw_recn, i);

        prep->change = 0;
    }

    hal_h264e_dbg_func("leave %p\n", hal);

    return MPP_OK;
}

static RK_U32 update_vepu580_syntax(HalH264eVepu580Ctx *ctx, MppSyntax *syntax)
{
    H264eSyntaxDesc *desc = syntax->data;
    RK_S32 syn_num = syntax->number;
    RK_U32 updated = 0;
    RK_S32 i;

    for (i = 0; i < syn_num; i++, desc++) {
        switch (desc->type) {
        case H264E_SYN_CFG : {
            hal_h264e_dbg_detail("update cfg");
            ctx->cfg = desc->p;
        } break;
        case H264E_SYN_SPS : {
            hal_h264e_dbg_detail("update sps");
            ctx->sps = desc->p;
        } break;
        case H264E_SYN_PPS : {
            hal_h264e_dbg_detail("update pps");
            ctx->pps = desc->p;
        } break;
        case H264E_SYN_SLICE : {
            hal_h264e_dbg_detail("update slice");
            ctx->slice = desc->p;
        } break;
        case H264E_SYN_FRAME : {
            hal_h264e_dbg_detail("update frames");
            ctx->frms = desc->p;
        } break;
        case H264E_SYN_PREFIX : {
            hal_h264e_dbg_detail("update prefix nal");
            ctx->prefix = desc->p;
        } break;
        default : {
            mpp_log_f("invalid syntax type %d\n", desc->type);
        } break;
        }

        updated |= SYN_TYPE_FLAG(desc->type);
    }

    return updated;
}

static MPP_RET hal_h264e_vepu580_get_task(void *hal, HalEncTask *task)
{
    HalH264eVepu580Ctx *ctx = (HalH264eVepu580Ctx *)hal;
    RK_U32 updated = update_vepu580_syntax(ctx, &task->syntax);
    EncFrmStatus *frm_status = &task->rc_task->frm;

    hal_h264e_dbg_func("enter %p\n", hal);

    if (updated & SYN_TYPE_FLAG(H264E_SYN_CFG))
        setup_hal_bufs(ctx);

    if (!frm_status->reencode && mpp_frame_has_meta(task->frame)) {
        MppMeta meta = mpp_frame_get_meta(task->frame);

        mpp_meta_get_ptr(meta, KEY_ROI_DATA, (void **)&ctx->roi_data);
        mpp_meta_get_ptr(meta, KEY_OSD_DATA, (void **)&ctx->osd_cfg.osd_data);
        mpp_meta_get_ptr(meta, KEY_OSD_DATA2, (void **)&ctx->osd_cfg.osd_data2);
    }
    hal_h264e_dbg_func("leave %p\n", hal);

    return MPP_OK;
}

static void setup_vepu580_normal(HalVepu580RegSet *regs)
{
    hal_h264e_dbg_func("enter\n");
    /* reg000 VERSION is read only */

    /* reg001 ENC_STRT */
    regs->reg_ctl.enc_strt.lkt_num            = 0;
    regs->reg_ctl.enc_strt.vepu_cmd          = 1;
    regs->reg_ctl.func_en.cke                = 1;
    regs->reg_ctl.func_en.resetn_hw_en       = 0;
    regs->reg_ctl.func_en.enc_done_tmvp_en   = 1;

    /* reg002 ENC_CLR */
    regs->reg_ctl.enc_clr.safe_clr           = 0;
    regs->reg_ctl.enc_clr.force_clr          = 0;

    /* reg003 LKT_ADDR */
    // regs->reg_ctl.lkt_addr           = 0;

    /* reg004 INT_EN */
    regs->reg_ctl.int_en.enc_done_en         = 1;
    regs->reg_ctl.int_en.lkt_node_done_en    = 1;
    regs->reg_ctl.int_en.sclr_done_en        = 1;
    regs->reg_ctl.int_en.slc_done_en         = 1;
    regs->reg_ctl.int_en.bsf_oflw_en         = 1;
    regs->reg_ctl.int_en.brsp_otsd_en        = 1;
    regs->reg_ctl.int_en.wbus_err_en         = 1;
    regs->reg_ctl.int_en.rbus_err_en         = 1;
    regs->reg_ctl.int_en.wdg_en              = 0;

    /* reg005 INT_MSK */
    regs->reg_ctl.int_msk.enc_done_msk       = 0;
    regs->reg_ctl.int_msk.lkt_node_done_msk  = 0;
    regs->reg_ctl.int_msk.sclr_done_msk      = 0;
    regs->reg_ctl.int_msk.slc_done_msk       = 0;
    regs->reg_ctl.int_msk.bsf_oflw_msk       = 0;
    regs->reg_ctl.int_msk.brsp_otsd_msk      = 0;
    regs->reg_ctl.int_msk.wbus_err_msk       = 0;
    regs->reg_ctl.int_msk.rbus_err_msk       = 0;
    regs->reg_ctl.int_msk.wdg_msk            = 0;

    /* reg006 INT_CLR is not set */
    /* reg007 INT_STA is read only */
    /* reg008 ~ reg0011 gap */
    regs->reg_ctl.enc_wdg.vs_load_thd        = 0;//x1ffff;
    regs->reg_ctl.enc_wdg.rfp_load_thd       = 0;//xff;

    /* reg015 DTRNS_MAP */
    regs->reg_ctl.dtrns_map.cmvw_bus_ordr      = 0;
    regs->reg_ctl.dtrns_map.dspw_bus_ordr      = 0;
    regs->reg_ctl.dtrns_map.rfpw_bus_ordr      = 0;
    regs->reg_ctl.dtrns_map.src_bus_edin       = 0;
    regs->reg_ctl.dtrns_map.meiw_bus_edin      = 0;
    regs->reg_ctl.dtrns_map.bsw_bus_edin       = 7;
    regs->reg_ctl.dtrns_map.lktr_bus_edin      = 0;
    regs->reg_ctl.dtrns_map.roir_bus_edin      = 0;
    regs->reg_ctl.dtrns_map.lktw_bus_edin      = 0;
    regs->reg_ctl.dtrns_map.afbc_bsize         = 1;

    regs->reg_ctl.dtrns_cfg.axi_brsp_cke   = 0;
    regs->reg_ctl.dtrns_cfg.dspr_otsd      = 1;
    hal_h264e_dbg_func("leave\n");
}

static MPP_RET setup_vepu580_prep(HalVepu580RegSet *regs, MppEncPrepCfg *prep)
{
    VepuFmtCfg cfg;
    MppFrameFormat fmt = prep->format;
    MPP_RET ret = vepu541_set_fmt(&cfg, fmt);
    RK_U32 hw_fmt = cfg.format;
    RK_S32 y_stride;
    RK_S32 c_stride;

    hal_h264e_dbg_func("enter\n");

    /* do nothing when color format is not supported */
    if (ret)
        return ret;

    regs->reg_base.enc_rsl.pic_wd8_m1 = MPP_ALIGN(prep->width, 16) / 8 - 1;
    regs->reg_base.src_fill.pic_wfill  = prep->width & 0xf;
    regs->reg_base.enc_rsl.pic_hd8_m1 = MPP_ALIGN(prep->height, 16) / 8 - 1;
    regs->reg_base.src_fill.pic_hfill  = prep->height & 0xf;

    regs->reg_ctl.dtrns_map.src_bus_edin = cfg.src_endian;

    regs->reg_base.src_fmt.src_cfmt   = hw_fmt;
    regs->reg_base.src_fmt.alpha_swap = cfg.alpha_swap;
    regs->reg_base.src_fmt.rbuv_swap  = cfg.rbuv_swap;
    regs->reg_base.src_fmt.src_range  = cfg.src_range;
    regs->reg_base.src_fmt.out_fmt    = 1;

    y_stride = (prep->hor_stride) ? (prep->hor_stride) : (prep->width);
    c_stride = (hw_fmt == VEPU541_FMT_YUV422SP || hw_fmt == VEPU541_FMT_YUV420SP) ?
               y_stride : y_stride / 2;

    if (hw_fmt < VEPU541_FMT_NONE) {
        regs->reg_base.src_udfy.csc_wgt_b2y    = 25;
        regs->reg_base.src_udfy.csc_wgt_g2y    = 129;
        regs->reg_base.src_udfy.csc_wgt_r2y    = 66;

        regs->reg_base.src_udfu.csc_wgt_b2u    = 112;
        regs->reg_base.src_udfu.csc_wgt_g2u    = -74;
        regs->reg_base.src_udfu.csc_wgt_r2u    = -38;

        regs->reg_base.src_udfv.csc_wgt_b2v    = -18;
        regs->reg_base.src_udfv.csc_wgt_g2v    = -94;
        regs->reg_base.src_udfv.csc_wgt_r2v    = 112;

        regs->reg_base.src_udfo.csc_ofst_y     = 15;
        regs->reg_base.src_udfo.csc_ofst_u     = 128;
        regs->reg_base.src_udfo.csc_ofst_v     = 128;
    } else {
        regs->reg_base.src_udfy.csc_wgt_b2y    = cfg.weight[0];
        regs->reg_base.src_udfy.csc_wgt_g2y    = cfg.weight[1];
        regs->reg_base.src_udfy.csc_wgt_r2y    = cfg.weight[2];


        regs->reg_base.src_udfu.csc_wgt_b2u    = cfg.weight[3];
        regs->reg_base.src_udfu.csc_wgt_g2u    = cfg.weight[4];
        regs->reg_base.src_udfu.csc_wgt_r2u    = cfg.weight[5];


        regs->reg_base.src_udfv.csc_wgt_b2v    = cfg.weight[6];
        regs->reg_base.src_udfv.csc_wgt_g2v    = cfg.weight[7];
        regs->reg_base.src_udfv.csc_wgt_r2v    = cfg.weight[8];


        regs->reg_base.src_udfo.csc_ofst_y     = cfg.offset[0];
        regs->reg_base.src_udfo.csc_ofst_u     = cfg.offset[1];
        regs->reg_base.src_udfo.csc_ofst_v     = cfg.offset[2];
    }

    regs->reg_base.src_proc.afbcd_en   = MPP_FRAME_FMT_IS_FBC(fmt) ? 1 : 0;
    regs->reg_base.src_strd0.src_strd0  = y_stride;
    regs->reg_base.src_strd1.src_strd1  = c_stride;

    regs->reg_base.src_proc.src_mirr   = prep->mirroring > 0;
    regs->reg_base.src_proc.src_rot    = prep->rotation;
    regs->reg_base.src_proc.txa_en     = 0;

    regs->reg_base.sli_cfg.sli_crs_en = 1;

    regs->reg_base.pic_ofst.pic_ofst_y = 0;
    regs->reg_base.pic_ofst.pic_ofst_x = 0;

    hal_h264e_dbg_func("leave\n");

    return ret;
}

static void setup_vepu580_codec(HalVepu580RegSet *regs, H264eSps *sps,
                                H264ePps *pps, H264eSlice *slice)
{
    hal_h264e_dbg_func("enter\n");

    regs->reg_base.enc_pic.enc_stnd       = 0;
    regs->reg_base.enc_pic.cur_frm_ref    = slice->nal_reference_idc > 0;
    regs->reg_base.enc_pic.bs_scp         = 1;
    //regs->reg013.lamb_mod_sel   = (slice->slice_type == H264_I_SLICE) ? 0 : 1;
    //regs->reg013.atr_thd_sel    = 0;
    // regs->reg_ctl.lkt_node_cfg.node_int       = 0;

    regs->reg_base.synt_nal.nal_ref_idc    = slice->nal_reference_idc;
    regs->reg_base.synt_nal.nal_unit_type  = slice->nalu_type;

    regs->reg_base.synt_sps.max_fnum       = sps->log2_max_frame_num_minus4;
    regs->reg_base.synt_sps.drct_8x8       = sps->direct8x8_inference;
    regs->reg_base.synt_sps.mpoc_lm4       = sps->log2_max_poc_lsb_minus4;

    regs->reg_base.synt_pps.etpy_mode      = pps->entropy_coding_mode;
    regs->reg_base.synt_pps.trns_8x8       = pps->transform_8x8_mode;
    regs->reg_base.synt_pps.csip_flag      = pps->constrained_intra_pred;
    regs->reg_base.synt_pps.num_ref0_idx   = pps->num_ref_idx_l0_default_active - 1;
    regs->reg_base.synt_pps.num_ref1_idx   = pps->num_ref_idx_l1_default_active - 1;
    regs->reg_base.synt_pps.pic_init_qp    = pps->pic_init_qp;
    regs->reg_base.synt_pps.cb_ofst        = pps->chroma_qp_index_offset;
    regs->reg_base.synt_pps.cr_ofst        = pps->second_chroma_qp_index_offset;
    regs->reg_base.synt_pps.wght_pred      = pps->weighted_pred;
    regs->reg_base.synt_pps.dbf_cp_flg     = pps->deblocking_filter_control;

    regs->reg_base.synt_sli0.sli_type       = (slice->slice_type == H264_I_SLICE) ? (2) : (0);
    regs->reg_base.synt_sli0.pps_id         = slice->pic_parameter_set_id;
    regs->reg_base.synt_sli0.drct_smvp      = 0;
    regs->reg_base.synt_sli0.num_ref_ovrd   = slice->num_ref_idx_override;
    regs->reg_base.synt_sli0.cbc_init_idc   = slice->cabac_init_idc;
    regs->reg_base.synt_sli0.frm_num        = slice->frame_num;

    regs->reg_base.synt_sli1.idr_pid        = (slice->slice_type == H264_I_SLICE) ? slice->idr_pic_id : (RK_U32)(-1);
    regs->reg_base.synt_sli1.poc_lsb        = slice->pic_order_cnt_lsb;


    regs->reg_base.synt_sli2.dis_dblk_idc   = slice->disable_deblocking_filter_idc;
    regs->reg_base.synt_sli2.sli_alph_ofst  = slice->slice_alpha_c0_offset_div2;

    h264e_reorder_rd_rewind(slice->reorder);
    {   /* reorder process */
        H264eRplmo rplmo;
        MPP_RET ret = h264e_reorder_rd_op(slice->reorder, &rplmo);

        if (MPP_OK == ret) {
            regs->reg_base.synt_sli2.ref_list0_rodr = 1;
            regs->reg_base.synt_sli2.rodr_pic_idx   = rplmo.modification_of_pic_nums_idc;

            switch (rplmo.modification_of_pic_nums_idc) {
            case 0 :
            case 1 : {
                regs->reg_base.synt_sli2.rodr_pic_num   = rplmo.abs_diff_pic_num_minus1;
            } break;
            case 2 : {
                regs->reg_base.synt_sli2.rodr_pic_num   = rplmo.long_term_pic_idx;
            } break;
            default : {
                mpp_err_f("invalid modification_of_pic_nums_idc %d\n",
                          rplmo.modification_of_pic_nums_idc);
            } break;
            }
        } else {
            // slice->ref_pic_list_modification_flag;
            regs->reg_base.synt_sli2.ref_list0_rodr = 0;
            regs->reg_base.synt_sli2.rodr_pic_idx   = 0;
            regs->reg_base.synt_sli2.rodr_pic_num   = 0;
        }
    }

    /* clear all mmco arg first */
    regs->reg_base.synt_refm0.nopp_flg               = 0;
    regs->reg_base.synt_refm0.ltrf_flg               = 0;
    regs->reg_base.synt_refm0.arpm_flg               = 0;
    regs->reg_base.synt_refm0.mmco4_pre              = 0;
    regs->reg_base.synt_refm0.mmco_type0             = 0;
    regs->reg_base.synt_refm0.mmco_parm0             = 0;
    regs->reg_base.synt_refm0.mmco_type1             = 0;
    regs->reg_base.synt_refm1.mmco_parm1             = 0;
    regs->reg_base.synt_refm0.mmco_type2             = 0;
    regs->reg_base.synt_refm1.mmco_parm2             = 0;
    regs->reg_base.synt_refm2.long_term_frame_idx0   = 0;
    regs->reg_base.synt_refm2.long_term_frame_idx1   = 0;
    regs->reg_base.synt_refm2.long_term_frame_idx2   = 0;

    h264e_marking_rd_rewind(slice->marking);

    /* only update used parameter */
    if (slice->slice_type == H264_I_SLICE) {
        regs->reg_base.synt_refm0.nopp_flg       = slice->no_output_of_prior_pics;
        regs->reg_base.synt_refm0.ltrf_flg       = slice->long_term_reference_flag;
    } else {
        if (!h264e_marking_is_empty(slice->marking)) {
            H264eMmco mmco;

            regs->reg_base.synt_refm0.arpm_flg       = 1;

            /* max 3 mmco */
            do {
                RK_S32 type = 0;
                RK_S32 param_0 = 0;
                RK_S32 param_1 = 0;

                h264e_marking_rd_op(slice->marking, &mmco);
                type = mmco.mmco;
                switch (type) {
                case 1 : {
                    param_0 = mmco.difference_of_pic_nums_minus1;
                } break;
                case 2 : {
                    param_0 = mmco.long_term_pic_num;
                } break;
                case 3 : {
                    param_0 = mmco.difference_of_pic_nums_minus1;
                    param_1 = mmco.long_term_frame_idx;
                } break;
                case 4 : {
                    param_0 = mmco.max_long_term_frame_idx_plus1;
                } break;
                case 5 : {
                } break;
                case 6 : {
                    param_0 = mmco.long_term_frame_idx;
                } break;
                default : {
                    mpp_err_f("unsupported mmco 0 %d\n", type);
                    type = 0;
                } break;
                }

                regs->reg_base.synt_refm0.mmco_type0 = type;
                regs->reg_base.synt_refm0.mmco_parm0 = param_0;
                regs->reg_base.synt_refm2.long_term_frame_idx0 = param_1;

                if (h264e_marking_is_empty(slice->marking))
                    break;

                h264e_marking_rd_op(slice->marking, &mmco);
                type = mmco.mmco;
                param_0 = 0;
                param_1 = 0;
                switch (type) {
                case 1 : {
                    param_0 = mmco.difference_of_pic_nums_minus1;
                } break;
                case 2 : {
                    param_0 = mmco.long_term_pic_num;
                } break;
                case 3 : {
                    param_0 = mmco.difference_of_pic_nums_minus1;
                    param_1 = mmco.long_term_frame_idx;
                } break;
                case 4 : {
                    param_0 = mmco.max_long_term_frame_idx_plus1;
                } break;
                case 5 : {
                } break;
                case 6 : {
                    param_0 = mmco.long_term_frame_idx;
                } break;
                default : {
                    mpp_err_f("unsupported mmco 0 %d\n", type);
                    type = 0;
                } break;
                }

                regs->reg_base.synt_refm0.mmco_type1 = type;
                regs->reg_base.synt_refm1.mmco_parm1 = param_0;
                regs->reg_base.synt_refm2.long_term_frame_idx1 = param_1;

                if (h264e_marking_is_empty(slice->marking))
                    break;

                h264e_marking_rd_op(slice->marking, &mmco);
                type = mmco.mmco;
                param_0 = 0;
                param_1 = 0;
                switch (type) {
                case 1 : {
                    param_0 = mmco.difference_of_pic_nums_minus1;
                } break;
                case 2 : {
                    param_0 = mmco.long_term_pic_num;
                } break;
                case 3 : {
                    param_0 = mmco.difference_of_pic_nums_minus1;
                    param_1 = mmco.long_term_frame_idx;
                } break;
                case 4 : {
                    param_0 = mmco.max_long_term_frame_idx_plus1;
                } break;
                case 5 : {
                } break;
                case 6 : {
                    param_0 = mmco.long_term_frame_idx;
                } break;
                default : {
                    mpp_err_f("unsupported mmco 0 %d\n", type);
                    type = 0;
                } break;
                }

                regs->reg_base.synt_refm0.mmco_type2 = type;
                regs->reg_base.synt_refm1.mmco_parm2 = param_0;
                regs->reg_base.synt_refm2.long_term_frame_idx2 = param_1;
            } while (0);
        }
    }

    hal_h264e_dbg_func("leave\n");
}

static void setup_vepu580_rdo_pred(HalVepu580RegSet *regs, H264eSps *sps,
                                   H264ePps *pps, H264eSlice *slice)
{
    hal_h264e_dbg_func("enter\n");

    if (slice->slice_type == H264_I_SLICE) {
        regs->reg_rc_klut.klut_ofst.chrm_klut_ofst = 0;
        memcpy(&regs->reg_rc_klut.klut_wgt0, &h264e_klut_weight[0], CHROMA_KLUT_TAB_SIZE);
    } else {
        regs->reg_rc_klut.klut_ofst.chrm_klut_ofst = 3;
        memcpy(&regs->reg_rc_klut.klut_wgt0, &h264e_klut_weight[4], CHROMA_KLUT_TAB_SIZE);
    }

    regs->reg_base.iprd_csts.vthd_y         = 9;
    regs->reg_base.iprd_csts.vthd_c         = 63;

    regs->reg_base.rdo_cfg.rect_size      = (sps->profile_idc == H264_PROFILE_BASELINE &&
                                             sps->level_idc <= H264_LEVEL_3_0) ? 1 : 0;
    regs->reg_base.rdo_cfg.inter_4x4      = 0;
    regs->reg_base.rdo_cfg.vlc_lmt        = (sps->profile_idc < H264_PROFILE_MAIN) &&
                                            !pps->entropy_coding_mode;
    regs->reg_base.rdo_cfg.chrm_spcl      = 1;
    regs->reg_base.rdo_cfg.rdo_mask       = 24;
    regs->reg_base.rdo_cfg.ccwa_e         = 1;
    regs->reg_base.rdo_cfg.scl_lst_sel    = pps->pic_scaling_matrix_present;
    regs->reg_base.rdo_cfg.atr_e          = 1;
    regs->reg_base.rdo_cfg.atf_intra_e    = 1;

    hal_h264e_dbg_func("leave\n");
}

static void setup_vepu580_rdo_cfg(Vepu580RdoCfg *regs)
{
    hal_h264e_dbg_func("enter\n");

    /* 0x2000 */
    regs->rdo_sqi_cfg.atf_pskip_en = 1;

    /* 0x20CC ~ 0x20D0 */
    regs->rdo_intra_cime_thd0.atf_rdo_intra_cime_thd0 = 20;
    regs->rdo_intra_cime_thd0.atf_rdo_intra_cime_thd1 = 40;
    regs->rdo_intra_cime_thd1.atf_rdo_intra_cime_thd2 = 72;

    /* 0x20D4 ~ 0x20E0 */
    regs->rdo_intra_var_thd0.atf_rdo_intra_var_thd00 = 25;
    regs->rdo_intra_var_thd0.atf_rdo_intra_var_thd01 = 64;
    regs->rdo_intra_var_thd1.atf_rdo_intra_var_thd10 = 25;
    regs->rdo_intra_var_thd1.atf_rdo_intra_var_thd11 = 64;
    regs->rdo_intra_var_thd2.atf_rdo_intra_var_thd20 = 70;
    regs->rdo_intra_var_thd2.atf_rdo_intra_var_thd21 = 100;
    regs->rdo_intra_var_thd3.atf_rdo_intra_var_thd30 = 70;
    regs->rdo_intra_var_thd3.atf_rdo_intra_var_thd31 = 100;

    /* 0x20E4 ~ 0x20F0 */
    regs->rdo_intra_atf_wgt0.atf_rdo_intra_wgt00 = 28;
    regs->rdo_intra_atf_wgt0.atf_rdo_intra_wgt01 = 27;
    regs->rdo_intra_atf_wgt0.atf_rdo_intra_wgt02 = 26;
    regs->rdo_intra_atf_wgt1.atf_rdo_intra_wgt10 = 26;
    regs->rdo_intra_atf_wgt1.atf_rdo_intra_wgt11 = 25;
    regs->rdo_intra_atf_wgt1.atf_rdo_intra_wgt12 = 24;
    regs->rdo_intra_atf_wgt2.atf_rdo_intra_wgt20 = 22;
    regs->rdo_intra_atf_wgt2.atf_rdo_intra_wgt21 = 20;
    regs->rdo_intra_atf_wgt2.atf_rdo_intra_wgt22 = 19;
    regs->rdo_intra_atf_wgt3.atf_rdo_intra_wgt30 = 16;
    regs->rdo_intra_atf_wgt3.atf_rdo_intra_wgt31 = 16;
    regs->rdo_intra_atf_wgt3.atf_rdo_intra_wgt32 = 16;

    /* 0x211C ~ 0x2130 */
    regs->rdo_skip_cime_thd0.atf_rdo_skip_cime_thd0 = 10;
    regs->rdo_skip_cime_thd0.atf_rdo_skip_cime_thd1 = 10;
    regs->rdo_skip_cime_thd1.atf_rdo_skip_cime_thd2 = 15;
    regs->rdo_skip_cime_thd1.atf_rdo_skip_cime_thd3 = 25;
    regs->rdo_skip_var_thd0.atf_rdo_skip_var_thd10 = 25;
    regs->rdo_skip_var_thd0.atf_rdo_skip_var_thd11 = 40;
    regs->rdo_skip_var_thd1.atf_rdo_skip_var_thd20 = 25;
    regs->rdo_skip_var_thd1.atf_rdo_skip_var_thd21 = 40;
    regs->rdo_skip_var_thd2.atf_rdo_skip_var_thd30 = 70;
    regs->rdo_skip_var_thd2.atf_rdo_skip_var_thd31 = 100;
    regs->rdo_skip_var_thd3.atf_rdo_skip_var_thd40 = 70;
    regs->rdo_skip_var_thd3.atf_rdo_skip_var_thd41 = 100;

    /* 0x2134 ~ 0x2140 */
    regs->rdo_skip_atf_wgt0.atf_rdo_skip_atf_wgt00 = 18;
    regs->rdo_skip_atf_wgt0.atf_rdo_skip_atf_wgt10 = 13;
    regs->rdo_skip_atf_wgt0.atf_rdo_skip_atf_wgt11 = 14;
    regs->rdo_skip_atf_wgt0.atf_rdo_skip_atf_wgt12 = 14;
    regs->rdo_skip_atf_wgt1.atf_rdo_skip_atf_wgt20 = 14;
    regs->rdo_skip_atf_wgt1.atf_rdo_skip_atf_wgt21 = 15;
    regs->rdo_skip_atf_wgt1.atf_rdo_skip_atf_wgt22 = 15;
    regs->rdo_skip_atf_wgt2.atf_rdo_skip_atf_wgt30 = 15;
    regs->rdo_skip_atf_wgt2.atf_rdo_skip_atf_wgt31 = 15;
    regs->rdo_skip_atf_wgt2.atf_rdo_skip_atf_wgt32 = 16;
    regs->rdo_skip_atf_wgt3.atf_rdo_skip_atf_wgt40 = 16;
    regs->rdo_skip_atf_wgt3.atf_rdo_skip_atf_wgt41 = 16;
    regs->rdo_skip_atf_wgt3.atf_rdo_skip_atf_wgt42 = 16;

    hal_h264e_dbg_func("leave\n");
}

static void setup_vepu580_rc_base(HalVepu580RegSet *regs, H264eSps *sps,
                                  H264eSlice *slice, MppEncHwCfg *hw,
                                  EncRcTask *rc_task)
{
    EncRcTaskInfo *rc_info = &rc_task->info;
    RK_S32 mb_w = sps->pic_width_in_mbs;
    RK_S32 mb_h = sps->pic_height_in_mbs;
    RK_U32 qp_target = rc_info->quality_target;
    RK_U32 qp_min = rc_info->quality_min;
    RK_U32 qp_max = rc_info->quality_max;
    RK_U32 qpmap_mode = 1;
    RK_S32 mb_target_bits_mul_16 = (rc_info->bit_target << 4) / (mb_w * mb_h);
    RK_S32 mb_target_bits;
    RK_S32 negative_bits_thd;
    RK_S32 positive_bits_thd;

    hal_h264e_dbg_rc("bittarget %d qp [%d %d %d]\n", rc_info->bit_target,
                     qp_min, qp_target, qp_max);

    if (mb_target_bits_mul_16 >= 0x100000) {
        mb_target_bits_mul_16 = 0x50000;
    }

    mb_target_bits = (mb_target_bits_mul_16 * mb_w) >> 4;
    negative_bits_thd = 0 - mb_target_bits / 4;
    positive_bits_thd = mb_target_bits / 4;

    hal_h264e_dbg_func("enter\n");

    regs->reg_base.enc_pic.pic_qp         = qp_target;

    regs->reg_base.rc_cfg.rc_en          = 1;
    regs->reg_base.rc_cfg.aq_en          = 1;
    regs->reg_base.rc_cfg.aq_mode        = 0;
    regs->reg_base.rc_cfg.rc_ctu_num     = mb_w;

    regs->reg_base.rc_qp.rc_qp_range    = (slice->slice_type == H264_I_SLICE) ?
                                          hw->qp_delta_row_i : hw->qp_delta_row;
    regs->reg_base.rc_qp.rc_max_qp      = qp_max;
    regs->reg_base.rc_qp.rc_min_qp      = qp_min;

    regs->reg_base.rc_tgt.ctu_ebit       = mb_target_bits_mul_16;

    regs->reg_rc_klut.rc_adj0.qp_adj0        = -1;
    regs->reg_rc_klut.rc_adj0.qp_adj1        = 0;
    regs->reg_rc_klut.rc_adj0.qp_adj2        = 0;
    regs->reg_rc_klut.rc_adj0.qp_adj3        = 0;
    regs->reg_rc_klut.rc_adj0.qp_adj4        = 0;
    regs->reg_rc_klut.rc_adj1.qp_adj5        = 0;
    regs->reg_rc_klut.rc_adj1.qp_adj6        = 0;
    regs->reg_rc_klut.rc_adj1.qp_adj7        = 0;
    regs->reg_rc_klut.rc_adj1.qp_adj8        = 1;

    regs->reg_rc_klut.rc_dthd_0_8[0] = negative_bits_thd;
    regs->reg_rc_klut.rc_dthd_0_8[1] = positive_bits_thd;
    regs->reg_rc_klut.rc_dthd_0_8[2] = positive_bits_thd;
    regs->reg_rc_klut.rc_dthd_0_8[3] = positive_bits_thd;
    regs->reg_rc_klut.rc_dthd_0_8[4] = positive_bits_thd;
    regs->reg_rc_klut.rc_dthd_0_8[5] = positive_bits_thd;
    regs->reg_rc_klut.rc_dthd_0_8[6] = positive_bits_thd;
    regs->reg_rc_klut.rc_dthd_0_8[7] = positive_bits_thd;
    regs->reg_rc_klut.rc_dthd_0_8[8] = positive_bits_thd;

    regs->reg_rc_klut.roi_qthd0.qpmin_area0    = qp_min;
    regs->reg_rc_klut.roi_qthd0.qpmax_area0    = qp_max;
    regs->reg_rc_klut.roi_qthd0.qpmin_area1    = qp_min;
    regs->reg_rc_klut.roi_qthd0.qpmax_area1    = qp_max;
    regs->reg_rc_klut.roi_qthd0.qpmin_area2    = qp_min;

    regs->reg_rc_klut.roi_qthd1.qpmax_area2    = qp_max;
    regs->reg_rc_klut.roi_qthd1.qpmin_area3    = qp_min;
    regs->reg_rc_klut.roi_qthd1.qpmax_area3    = qp_max;
    regs->reg_rc_klut.roi_qthd1.qpmin_area4    = qp_min;
    regs->reg_rc_klut.roi_qthd1.qpmax_area4    = qp_max;

    regs->reg_rc_klut.roi_qthd2.qpmin_area5    = qp_min;
    regs->reg_rc_klut.roi_qthd2.qpmax_area5    = qp_max;
    regs->reg_rc_klut.roi_qthd2.qpmin_area6    = qp_min;
    regs->reg_rc_klut.roi_qthd2.qpmax_area6    = qp_max;
    regs->reg_rc_klut.roi_qthd2.qpmin_area7    = qp_min;

    regs->reg_rc_klut.roi_qthd3.qpmax_area7    = qp_max;
    regs->reg_rc_klut.roi_qthd3.qpmap_mode     = qpmap_mode;

    {
        /* 0x1070 ~ 0x1074 */
        regs->reg_rc_klut.md_sad_thd.md_sad_thd0 = 25;
        regs->reg_rc_klut.md_sad_thd.md_sad_thd1 = 25;
        regs->reg_rc_klut.md_sad_thd.md_sad_thd2 = 25;

        regs->reg_rc_klut.madi_thd.madi_thd0 = 25;
        regs->reg_rc_klut.madi_thd.madi_thd1 = 25;
        regs->reg_rc_klut.madi_thd.madi_thd2 = 25;
    }

    hal_h264e_dbg_func("leave\n");
}

static void setup_vepu580_io_buf(HalVepu580RegSet *regs, MppDev dev,
                                 HalEncTask *task)
{
    MppFrame frm = task->frame;
    MppPacket pkt = task->packet;
    MppBuffer buf_in = mpp_frame_get_buffer(frm);
    MppBuffer buf_out = task->output;
    MppFrameFormat fmt = mpp_frame_get_fmt(frm);
    RK_S32 hor_stride = mpp_frame_get_hor_stride(frm);
    RK_S32 ver_stride = mpp_frame_get_ver_stride(frm);
    RK_S32 fd_in = mpp_buffer_get_fd(buf_in);
    RK_U32 off_in[2] = {0};
    RK_U32 off_out = mpp_packet_get_length(pkt);
    size_t siz_out = mpp_buffer_get_size(buf_out);
    RK_S32 fd_out = mpp_buffer_get_fd(buf_out);

    hal_h264e_dbg_func("enter\n");

    regs->reg_base.adr_src0   = fd_in;
    regs->reg_base.adr_src1   = fd_in;
    regs->reg_base.adr_src2   = fd_in;

    regs->reg_base.bsbb_addr  = fd_out;
    regs->reg_base.bsbr_addr  = fd_out;
    regs->reg_base.adr_bsbs   = fd_out;
    regs->reg_base.bsbt_addr  = fd_out;

    if (MPP_FRAME_FMT_IS_FBC(fmt)) {
        off_in[0] = mpp_frame_get_fbc_offset(frm);;
        off_in[1] = 0;
    } else if (MPP_FRAME_FMT_IS_YUV(fmt)) {
        VepuFmtCfg cfg;

        vepu541_set_fmt(&cfg, fmt);
        switch (cfg.format) {
        case VEPU541_FMT_BGRA8888 :
        case VEPU541_FMT_BGR888 :
        case VEPU541_FMT_BGR565 : {
            off_in[0] = 0;
            off_in[1] = 0;
        } break;
        case VEPU541_FMT_YUV420SP :
        case VEPU541_FMT_YUV422SP : {
            off_in[0] = hor_stride * ver_stride;
            off_in[1] = hor_stride * ver_stride;
        } break;
        case VEPU541_FMT_YUV422P : {
            off_in[0] = hor_stride * ver_stride;
            off_in[1] = hor_stride * ver_stride * 3 / 2;
        } break;
        case VEPU541_FMT_YUV420P : {
            off_in[0] = hor_stride * ver_stride;
            off_in[1] = hor_stride * ver_stride * 5 / 4;
        } break;
        case VEPU541_FMT_YUYV422 :
        case VEPU541_FMT_UYVY422 : {
            off_in[0] = 0;
            off_in[1] = 0;
        } break;
        case VEPU541_FMT_NONE :
        default : {
            off_in[0] = 0;
            off_in[1] = 0;
        } break;
        }
    }

    MppDevRegOffsetCfg trans_cfg;

    trans_cfg.reg_idx = 161;
    trans_cfg.offset = off_in[0];
    mpp_dev_ioctl(dev, MPP_DEV_REG_OFFSET, &trans_cfg);

    trans_cfg.reg_idx = 162;
    trans_cfg.offset = off_in[1];
    mpp_dev_ioctl(dev, MPP_DEV_REG_OFFSET, &trans_cfg);

    trans_cfg.reg_idx = 172;
    trans_cfg.offset = siz_out;
    mpp_dev_ioctl(dev, MPP_DEV_REG_OFFSET, &trans_cfg);

    trans_cfg.reg_idx = 175;
    trans_cfg.offset = off_out;
    mpp_dev_ioctl(dev, MPP_DEV_REG_OFFSET, &trans_cfg);

    hal_h264e_dbg_func("leave\n");
}

static void setup_vepu580_roi(HalVepu580RegSet *regs, HalH264eVepu580Ctx *ctx)
{
    MppEncROICfg *roi = ctx->roi_data;
    RK_U32 w = ctx->sps->pic_width_in_mbs * 16;
    RK_U32 h = ctx->sps->pic_height_in_mbs * 16;

    hal_h264e_dbg_func("enter\n");

    /* roi setup */
    if (roi && roi->number && roi->regions) {
        RK_S32 roi_buf_size = vepu541_get_roi_buf_size(w, h);

        if (!ctx->roi_buf || roi_buf_size != ctx->roi_buf_size) {
            if (NULL == ctx->roi_grp)
                mpp_buffer_group_get_internal(&ctx->roi_grp, MPP_BUFFER_TYPE_ION);
            else if (roi_buf_size != ctx->roi_buf_size) {
                if (ctx->roi_buf) {
                    mpp_buffer_put(ctx->roi_buf);
                    ctx->roi_buf = NULL;
                }
                mpp_buffer_group_clear(ctx->roi_grp);
            }

            mpp_assert(ctx->roi_grp);

            if (NULL == ctx->roi_buf)
                mpp_buffer_get(ctx->roi_grp, &ctx->roi_buf, roi_buf_size);

            ctx->roi_buf_size = roi_buf_size;
        }

        mpp_assert(ctx->roi_buf);
        RK_S32 fd = mpp_buffer_get_fd(ctx->roi_buf);
        void *buf = mpp_buffer_get_ptr(ctx->roi_buf);

        regs->reg_base.enc_pic.roi_en = 1;
        regs->reg_base.roi_addr = fd;

        vepu541_set_roi(buf, roi, w, h);
    } else {
        regs->reg_base.enc_pic.roi_en = 0;
        regs->reg_base.roi_addr = 0;
    }

    hal_h264e_dbg_func("leave\n");
}

static void setup_vepu580_recn_refr(HalVepu580RegSet *regs, MppDev dev,
                                    H264eFrmInfo *frms, HalBufs bufs,
                                    RK_S32 fbc_hdr_size)
{
    HalBuf *curr = hal_bufs_get_buf(bufs, frms->curr_idx);
    HalBuf *refr = hal_bufs_get_buf(bufs, frms->refr_idx);
    MppDevRegOffsetCfg trans_cfg;

    hal_h264e_dbg_func("enter\n");

    if (curr && curr->cnt) {
        MppBuffer buf_pixel = curr->buf[0];
        MppBuffer buf_thumb = curr->buf[1];
        RK_S32 fd = mpp_buffer_get_fd(buf_pixel);

        mpp_assert(buf_pixel);
        mpp_assert(buf_thumb);

        regs->reg_base.rfpw_h_addr = fd;
        regs->reg_base.rfpw_b_addr = fd;
        regs->reg_base.dspw_addr = mpp_buffer_get_fd(buf_thumb);

        trans_cfg.reg_idx = 164;
        trans_cfg.offset = fbc_hdr_size;
        mpp_dev_ioctl(dev, MPP_DEV_REG_OFFSET, &trans_cfg);
    }

    if (refr && refr->cnt) {
        MppBuffer buf_pixel = refr->buf[0];
        MppBuffer buf_thumb = refr->buf[1];
        RK_S32 fd = mpp_buffer_get_fd(buf_pixel);

        mpp_assert(buf_pixel);
        mpp_assert(buf_thumb);

        regs->reg_base.rfpr_h_addr = fd;
        regs->reg_base.rfpr_b_addr = fd;
        regs->reg_base.dspr_addr = mpp_buffer_get_fd(buf_thumb);

        trans_cfg.reg_idx = 166;
        trans_cfg.offset = fbc_hdr_size;
        mpp_dev_ioctl(dev, MPP_DEV_REG_OFFSET, &trans_cfg);
    }

    hal_h264e_dbg_func("leave\n");
}

static void setup_vepu580_split(HalVepu580RegSet *regs, MppEncSliceSplit *cfg)
{
    hal_h264e_dbg_func("enter\n");

    switch (cfg->split_mode) {
    case MPP_ENC_SPLIT_NONE : {
        regs->reg_base.sli_splt.sli_splt = 0;
        regs->reg_base.sli_splt.sli_splt_mode = 0;
        regs->reg_base.sli_splt.sli_splt_cpst = 0;
        regs->reg_base.sli_splt.sli_max_num_m1 = 0;
        regs->reg_base.sli_splt.sli_flsh = 0;
        regs->reg_base.sli_cnum.sli_splt_cnum_m1 = 0;

        regs->reg_base.sli_byte.sli_splt_byte = 0;
        regs->reg_base.enc_pic.slen_fifo = 0;
    } break;
    case MPP_ENC_SPLIT_BY_BYTE : {
        regs->reg_base.sli_splt.sli_splt = 1;
        regs->reg_base.sli_splt.sli_splt_mode = 0;
        regs->reg_base.sli_splt.sli_splt_cpst = 0;
        regs->reg_base.sli_splt.sli_max_num_m1 = 500;
        regs->reg_base.sli_splt.sli_flsh = 1;
        regs->reg_base.sli_cnum.sli_splt_cnum_m1 = 0;

        regs->reg_base.sli_byte.sli_splt_byte = cfg->split_arg;
        regs->reg_base.enc_pic.slen_fifo = 0;
    } break;
    case MPP_ENC_SPLIT_BY_CTU : {
        regs->reg_base.sli_splt.sli_splt = 1;
        regs->reg_base.sli_splt.sli_splt_mode = 1;
        regs->reg_base.sli_splt.sli_splt_cpst = 0;
        regs->reg_base.sli_splt.sli_max_num_m1 = 500;
        regs->reg_base.sli_splt.sli_flsh = 1;
        regs->reg_base.sli_cnum.sli_splt_cnum_m1 = cfg->split_arg - 1;

        regs->reg_base.sli_byte.sli_splt_byte = 0;
        regs->reg_base.enc_pic.slen_fifo = 0;
    } break;
    default : {
        mpp_log_f("invalide slice split mode %d\n", cfg->split_mode);
    } break;
    }

    cfg->change = 0;

    hal_h264e_dbg_func("leave\n");
}

static void setup_vepu580_force_slice_split(HalVepu580RegSet *regs, RK_S32 width)
{
    RK_S32 mb_w = MPP_ALIGN(width, 16) >> 4;

    hal_h264e_dbg_func("enter\n");

    regs->reg_base.sli_splt.sli_splt = 1;
    regs->reg_base.sli_splt.sli_splt_mode = 1;
    regs->reg_base.sli_splt.sli_splt_cpst = 0;
    regs->reg_base.sli_splt.sli_max_num_m1 = 500;
    regs->reg_base.sli_splt.sli_flsh = 1;
    regs->reg_base.sli_cnum.sli_splt_cnum_m1 = mb_w - 1;

    regs->reg_base.sli_byte.sli_splt_byte = 0;
    regs->reg_base.enc_pic.slen_fifo = 0;
    regs->reg_base.sli_cfg.sli_crs_en = 0;

    hal_h264e_dbg_func("leave\n");
}

static void calc_cime_parameter(HalVepu580RegSet *regs, H264eSps *sps)
{
    Vepu580BaseCfg *base_regs = &regs->reg_base;
    RK_S32 x_gmv = base_regs->gmv.gmv_x;
    RK_S32 y_gmv = base_regs->gmv.gmv_y;
    RK_S32 srch_w = base_regs->me_rnge.cme_srch_h * 4;
    RK_S32 srch_h = base_regs->me_rnge.cme_srch_v * 4;
    RK_S32 frm_sta = 0, frm_end = 0, pic_w = 0;
    RK_S32 pic_wd64 = ((sps->pic_width_in_mbs + 1) * 8 + 63) / 64;//((base_regs->pic_wd8_m1 + 1) * 8 + 63) / 64;

    // calc cime_linebuf_w
    {
        if (x_gmv - srch_w < 0) {
            frm_sta = (x_gmv - srch_w - 15) / 16;
        } else {
            frm_sta = (x_gmv - srch_w) / 16;
        }
        frm_sta = mpp_clip(frm_sta, 0, pic_wd64 - 1);

        if (x_gmv + srch_w < 0) {
            frm_end = pic_wd64 - 1 + (x_gmv + srch_w) / 16;
        } else {
            frm_end = pic_wd64 - 1 + (x_gmv + srch_w + 15) / 16;
        }
        frm_end = mpp_clip(frm_end, 0, pic_wd64 - 1);

        pic_w = (frm_end - frm_sta + 1) * 64;
        base_regs->me_cach.cme_linebuf_w = (pic_w ? pic_w : 64) / 64;
    }

    // calc cime_cacha_h and cime_cacha_max
    {
        RK_U32 cime_cacha_max = 2464;
        RK_U32 ctu_4_h = 1, ramb_h;
        RK_U32 cur_srch_16_w, cur_srch_4_h, cur_srch_max;
        RK_U32 cime_cacha_h = ctu_4_h;

        if ((x_gmv % 16 - srch_w % 16) < 0) {
            cur_srch_16_w = (16 + (x_gmv % 16 - srch_w % 16) % 16 + srch_w * 2 + 15) / 16 + 1;
        } else {
            cur_srch_16_w = ((x_gmv % 16 - srch_w % 16) % 16 + srch_w * 2 + 15) / 16 + 1;
        }

        if ((y_gmv %  4 - srch_h %  4) < 0) {
            cur_srch_4_h  = (4 + (y_gmv %  4 - srch_h %  4) %  4 + srch_h * 2 +  3) / 4  + ctu_4_h;
        } else {
            cur_srch_4_h  = ((y_gmv %  4 - srch_h %  4) %  4 + srch_h * 2 +  3) / 4  + ctu_4_h;
        }

        cur_srch_max = cur_srch_4_h;

        if (base_regs->me_cach.cme_linebuf_w < cur_srch_16_w) {
            cur_srch_16_w = base_regs->me_cach.cme_linebuf_w;
        }

        ramb_h = cur_srch_4_h;
        while ((cime_cacha_h < cur_srch_max) && (cime_cacha_max >
                                                 ((cime_cacha_h - ctu_4_h) * base_regs->me_cach.cme_linebuf_w * 4 + (ramb_h * 4 * cur_srch_16_w)))) {
            cime_cacha_h = cime_cacha_h + ctu_4_h;

            if (ramb_h > 2 * ctu_4_h) {
                ramb_h = ramb_h - ctu_4_h;
            } else {
                ramb_h = ctu_4_h;
            }
        }

        if (cur_srch_4_h == ctu_4_h) {
            cime_cacha_h = cime_cacha_h + ctu_4_h;
            ramb_h = 0;
        }

        if (cime_cacha_max < ((cime_cacha_h - ctu_4_h) * base_regs->me_cach.cme_linebuf_w * 4 + (ramb_h * 4 * cur_srch_16_w))) {
            cime_cacha_h = cime_cacha_h - ctu_4_h;
        }
        base_regs->me_cach.cme_rama_h = cime_cacha_h;

        /* cime_cacha_max */
        {
            RK_U32 ram_col_h = (cime_cacha_h - ctu_4_h) / ctu_4_h;
            base_regs->me_cach.cme_rama_max = ram_col_h * base_regs->me_cach.cme_linebuf_w + cur_srch_16_w;
        }
    }
}

static void setup_vepu580_me(HalVepu580RegSet *regs, H264eSps *sps,
                             H264eSlice *slice)
{
    RK_S32 level_idc = sps->level_idc;
    RK_S32 cime_w = 176;
    RK_S32 cime_h = 112;
    RK_S32 cime_blk_w_max = 44;
    RK_S32 cime_blk_h_max = 28;

    hal_h264e_dbg_func("enter\n");
    /*
     * Step 1. limit the mv range by level_idc
     * For level 1 and level 1b the vertical MV range is [-64,+63.75]
     * For level 1.1, 1.2, 1.3 and 2 the vertical MV range is [-128,+127.75]
     */
    switch (level_idc) {
    case H264_LEVEL_1_0 :
    case H264_LEVEL_1_b : {
        cime_blk_h_max = 12;
    } break;
    case H264_LEVEL_1_1 :
    case H264_LEVEL_1_2 :
    case H264_LEVEL_1_3 :
    case H264_LEVEL_2_0 : {
        cime_blk_h_max = 28;
    } break;
    default : {
        cime_blk_h_max = 28;
    } break;
    }

    if (cime_w < cime_blk_w_max * 4)
        cime_blk_w_max = cime_w / 4;

    if (cime_h < cime_blk_h_max * 4)
        cime_blk_h_max = cime_h / 4;

    /*
     * Step 2. limit the mv range by image size
     */
    if (cime_blk_w_max / 4 * 2 > (sps->pic_width_in_mbs * 2 + 1) / 2)
        cime_blk_w_max = (sps->pic_width_in_mbs * 2 + 1) / 2 / 2 * 4;

    if (cime_blk_h_max / 4 > MPP_ALIGN(sps->pic_height_in_mbs * 16, 64) / 128 * 4)
        cime_blk_h_max = MPP_ALIGN(sps->pic_height_in_mbs * 16, 64) / 128 * 16;

    regs->reg_base.me_rnge.cme_srch_h     = cime_blk_w_max / 4;
    regs->reg_base.me_rnge.cme_srch_v     = cime_blk_h_max / 4;
    regs->reg_base.me_rnge.rme_srch_h     = 7;
    regs->reg_base.me_rnge.rme_srch_v     = 5;
    regs->reg_base.me_rnge.dlt_frm_num    = 0;

    if (slice->slice_type == H264_I_SLICE) {
        regs->reg_base.me_cfg.pmv_mdst_h = 0;
        regs->reg_base.me_cfg.pmv_mdst_v = 0;
    } else {
        regs->reg_base.me_cfg.pmv_mdst_h = 5;
        regs->reg_base.me_cfg.pmv_mdst_v = 5;
    }
    regs->reg_base.me_cfg.mv_limit  = (sps->level_idc > 20) ? 2 : ((sps->level_idc >= 11) ? 1 : 0);//2;
    regs->reg_base.me_cfg.pmv_num   = 2;
    regs->reg_base.me_cfg.rme_dis   = 0;
    regs->reg_base.me_cfg.fme_dis   = 0;
    regs->reg_base.me_cfg.lvl4_ovrd_en   = 0;

    calc_cime_parameter(regs, sps);

    hal_h264e_dbg_func("leave\n");
}

#define H264E_LAMBDA_TAB_SIZE       (52 * sizeof(RK_U32))

static RK_U32 h264e_lambda_default[58] = {
    0x00000003, 0x00000005, 0x00000006, 0x00000007,
    0x00000009, 0x0000000b, 0x0000000e, 0x00000012,
    0x00000016, 0x0000001c, 0x00000024, 0x0000002d,
    0x00000039, 0x00000048, 0x0000005b, 0x00000073,
    0x00000091, 0x000000b6, 0x000000e6, 0x00000122,
    0x0000016d, 0x000001cc, 0x00000244, 0x000002db,
    0x00000399, 0x00000489, 0x000005b6, 0x00000733,
    0x00000912, 0x00000b6d, 0x00000e66, 0x00001224,
    0x000016db, 0x00001ccc, 0x00002449, 0x00002db7,
    0x00003999, 0x00004892, 0x00005b6f, 0x00007333,
    0x00009124, 0x0000b6de, 0x0000e666, 0x00012249,
    0x00016dbc, 0x0001cccc, 0x00024492, 0x0002db79,
    0x00039999, 0x00048924, 0x0005b6f2, 0x00073333,
    0x00091249, 0x000b6de5, 0x000e6666, 0x00122492,
    0x0016dbcb, 0x001ccccc,
};

static void setup_vepu580_l2(HalVepu580RegSet *regs, H264eSlice *slice)
{
    RK_U32 i;

    hal_h264e_dbg_func("enter\n");

    regs->reg_s3.iprd_wgt_qp_hevc_0_51[0] = 0;
    /* ~ */
    regs->reg_s3.iprd_wgt_qp_hevc_0_51[51] = 0;

    memcpy(regs->reg_s3.rdo_wgta_qp_grpa_0_51, &h264e_lambda_default[6], H264E_LAMBDA_TAB_SIZE);
    memset(regs->reg_s3.iprd_wgt_qp_hevc_0_51, 0, H264E_LAMBDA_TAB_SIZE);

    regs->reg_rc_klut.madi_cfg.madi_mode = 0;
    regs->reg_rc_klut.madi_cfg.madi_thd  = 25;

    regs->reg_s3.lvl32_intra_CST_THD0.lvl4_intra_cst_thd0 = 1;
    regs->reg_s3.lvl32_intra_CST_THD0.lvl4_intra_cst_thd1 = 4;
    regs->reg_s3.lvl32_intra_CST_THD1.lvl4_intra_cst_thd2 = 9;
    regs->reg_s3.lvl32_intra_CST_THD1.lvl4_intra_cst_thd3 = 36;

    regs->reg_s3.lvl16_intra_CST_THD0.lvl8_intra_chrm_cst_thd0 = 1;
    regs->reg_s3.lvl16_intra_CST_THD0.lvl8_intra_chrm_cst_thd1 = 4;
    regs->reg_s3.lvl16_intra_CST_THD1.lvl8_intra_chrm_cst_thd2 = 9;
    regs->reg_s3.lvl16_intra_CST_THD1.lvl8_intra_chrm_cst_thd3 = 36;

    regs->reg_s3.lvl8_intra_CST_THD0.lvl8_intra_cst_thd0 = 1;
    regs->reg_s3.lvl8_intra_CST_THD0.lvl8_intra_cst_thd1 = 4;
    regs->reg_s3.lvl8_intra_CST_THD1.lvl8_intra_cst_thd2 = 9;
    regs->reg_s3.lvl8_intra_CST_THD1.lvl8_intra_cst_thd3 = 36;

    regs->reg_s3.lvl16_intra_UL_CST_THD.lvl16_intra_ul_cst_thld = 0;
    regs->reg_s3.lvl32_intra_CST_WGT0.lvl8_intra_cst_wgt0 = 48;
    regs->reg_s3.lvl32_intra_CST_WGT0.lvl8_intra_cst_wgt1 = 60;
    regs->reg_s3.lvl32_intra_CST_WGT0.lvl8_intra_cst_wgt2 = 40;
    regs->reg_s3.lvl32_intra_CST_WGT0.lvl8_intra_cst_wgt3 = 48;

    regs->reg_s3.lvl32_intra_CST_WGT1.lvl4_intra_cst_wgt0 = 48;
    regs->reg_s3.lvl32_intra_CST_WGT1.lvl4_intra_cst_wgt1 = 60;
    regs->reg_s3.lvl32_intra_CST_WGT1.lvl4_intra_cst_wgt2 = 40;
    regs->reg_s3.lvl32_intra_CST_WGT1.lvl4_intra_cst_wgt3 = 48;

    regs->reg_s3.lvl16_intra_CST_WGT0.lvl16_intra_cst_wgt0 = 48;
    regs->reg_s3.lvl16_intra_CST_WGT0.lvl16_intra_cst_wgt1 = 60;
    regs->reg_s3.lvl16_intra_CST_WGT0.lvl16_intra_cst_wgt2 = 40;
    regs->reg_s3.lvl16_intra_CST_WGT0.lvl16_intra_cst_wgt3 = 48;
    /* 0x1728 */
    regs->reg_s3.lvl16_intra_CST_WGT1.lvl8_intra_chrm_cst_wgt0 = 36;
    regs->reg_s3.lvl16_intra_CST_WGT1.lvl8_intra_chrm_cst_wgt1 = 42;
    regs->reg_s3.lvl16_intra_CST_WGT1.lvl8_intra_chrm_cst_wgt2 = 28;
    regs->reg_s3.lvl16_intra_CST_WGT1.lvl8_intra_chrm_cst_wgt3 = 32;

    regs->reg_s3.RDO_QUANT.quant_f_bias_I = 683;
    regs->reg_s3.RDO_QUANT.quant_f_bias_P = 341;

    if (slice->slice_type == H264_I_SLICE) {
        regs->reg_s3.ATR_THD0.atr_thd0 = 1;
        regs->reg_s3.ATR_THD0.atr_thd1 = 4;
        regs->reg_s3.ATR_THD1.atr_thd2 = 36;
    } else {
        regs->reg_s3.ATR_THD0.atr_thd0 = 1;
        regs->reg_s3.ATR_THD0.atr_thd1 = 4;
        regs->reg_s3.ATR_THD1.atr_thd2 = 49;
    }
    regs->reg_s3.ATR_THD1.atr_thdqp = 32;

    if (slice->slice_type == H264_I_SLICE) {
        regs->reg_s3.Lvl16_ATR_WGT.lvl16_atr_wgt0 = 16;
        regs->reg_s3.Lvl16_ATR_WGT.lvl16_atr_wgt1 = 16;
        regs->reg_s3.Lvl16_ATR_WGT.lvl16_atr_wgt2 = 16;

        regs->reg_s3.Lvl8_ATR_WGT.lvl8_atr_wgt0 = 32;
        regs->reg_s3.Lvl8_ATR_WGT.lvl8_atr_wgt1 = 32;
        regs->reg_s3.Lvl8_ATR_WGT.lvl8_atr_wgt2 = 32;

        regs->reg_s3.Lvl4_ATR_WGT.lvl4_atr_wgt0 = 20;
        regs->reg_s3.Lvl4_ATR_WGT.lvl4_atr_wgt1 = 18;
        regs->reg_s3.Lvl4_ATR_WGT.lvl4_atr_wgt2 = 16;
    } else {
        regs->reg_s3.Lvl16_ATR_WGT.lvl16_atr_wgt0 = 16;
        regs->reg_s3.Lvl16_ATR_WGT.lvl16_atr_wgt1 = 17;
        regs->reg_s3.Lvl16_ATR_WGT.lvl16_atr_wgt2 = 17;

        regs->reg_s3.Lvl8_ATR_WGT.lvl8_atr_wgt0 = 31;
        regs->reg_s3.Lvl8_ATR_WGT.lvl8_atr_wgt1 = 31;
        regs->reg_s3.Lvl8_ATR_WGT.lvl8_atr_wgt2 = 31;

        regs->reg_s3.Lvl4_ATR_WGT.lvl4_atr_wgt0 = 21;
        regs->reg_s3.Lvl4_ATR_WGT.lvl4_atr_wgt1 = 19;
        regs->reg_s3.Lvl4_ATR_WGT.lvl4_atr_wgt2 = 17;
    }
    /* CIME */
    {
        /* 0x1760 */
        regs->reg_s3.cime_sqi_cfg.cime_sad_mod_sel = 0;
        regs->reg_s3.cime_sqi_cfg.cime_sad_use_big_block = 1;
        regs->reg_s3.cime_sqi_cfg.cime_pmv_set_zero = 1;
        regs->reg_s3.cime_sqi_cfg.cime_pmv_num = 3;

        /* 0x1764 */
        regs->reg_s3.cime_sqi_thd.cime_mvd_th0 = 32;
        regs->reg_s3.cime_sqi_thd.cime_mvd_th1 = 80;
        regs->reg_s3.cime_sqi_thd.cime_mvd_th2 = 128;

        /* 0x1768 */
        regs->reg_s3.cime_sqi_multi0.cime_multi0 = 16;
        regs->reg_s3.cime_sqi_multi0.cime_multi1 = 32;
        regs->reg_s3.cime_sqi_multi1.cime_multi2 = 96;
        regs->reg_s3.cime_sqi_multi1.cime_multi3 = 96;
    }

    /* RIME && FME */
    {
        /* 0x1770 */
        regs->reg_s3.rime_sqi_thd.cime_sad_th0  = 50;
        regs->reg_s3.rime_sqi_thd.rime_mvd_th0  = 3;
        regs->reg_s3.rime_sqi_thd.rime_mvd_th1  = 8;
        regs->reg_s3.rime_sqi_multi.rime_multi0 = 16;
        regs->reg_s3.rime_sqi_multi.rime_multi1 = 16;
        regs->reg_s3.rime_sqi_multi.rime_multi2 = 128;

        /* 0x1778 */
        regs->reg_s3.fme_sqi_thd0.cime_sad_pu16_th = 30;

        /* 0x177C */
        regs->reg_s3.fme_sqi_thd1.move_lambda = 1;
    }

    mpp_env_get_u32("dump_l2_reg", &dump_l2_reg, 0);

    if (dump_l2_reg) {
        mpp_log("L2 reg dump start:\n");
        RK_U32 *p = (RK_U32 *)regs;

        for (i = 0; i < (sizeof(*regs) / sizeof(RK_U32)); i++)
            mpp_log("%04x %08x\n", 4 + i * 4, p[i]);

        mpp_log("L2 reg done\n");
    }

    hal_h264e_dbg_func("leave\n");
}

static void setup_vepu580_ext_line_buf(HalVepu580RegSet *regs, HalH264eVepu580Ctx *ctx)
{
    if (ctx->ext_line_buf) {
        MppDevRegOffsetCfg trans_cfg;
        RK_S32 fd = mpp_buffer_get_fd(ctx->ext_line_buf);

        regs->reg_base.ebuft_addr = fd;
        regs->reg_base.ebufb_addr = fd;

        trans_cfg.reg_idx = 183;
        trans_cfg.offset = ctx->ext_line_buf_size;
        mpp_dev_ioctl(ctx->dev, MPP_DEV_REG_OFFSET, &trans_cfg);
    } else {
        regs->reg_base.ebufb_addr = 0;
        regs->reg_base.ebufb_addr = 0;
    }
}

static MPP_RET hal_h264e_vepu580_gen_regs(void *hal, HalEncTask *task)
{
    HalH264eVepu580Ctx *ctx = (HalH264eVepu580Ctx *)hal;
    HalVepu580RegSet *regs = &ctx->regs_set;
    MppEncCfgSet *cfg = ctx->cfg;
    MppEncPrepCfg *prep = &cfg->prep;
    H264eSps *sps = ctx->sps;
    H264ePps *pps = ctx->pps;
    H264eSlice *slice = ctx->slice;
    MPP_RET ret = MPP_OK;

    hal_h264e_dbg_func("enter %p\n", hal);
    hal_h264e_dbg_detail("frame %d generate regs now", ctx->frms->seq_idx);

    /* register setup */
    memset(regs, 0, sizeof(*regs));

    setup_vepu580_normal(regs);
    ret = setup_vepu580_prep(regs, &ctx->cfg->prep);
    if (ret)
        return ret;

    setup_vepu580_codec(regs, sps, pps, slice);
    setup_vepu580_rdo_pred(regs, sps, pps, slice);
    setup_vepu580_rdo_cfg(&regs->reg_rdo);
    setup_vepu580_rc_base(regs, sps, slice, &cfg->hw, task->rc_task);
    setup_vepu580_io_buf(regs, ctx->dev, task);
    setup_vepu580_roi(regs, ctx);
    setup_vepu580_recn_refr(regs, ctx->dev, ctx->frms, ctx->hw_recn,
                            ctx->pixel_buf_fbc_hdr_size);

    regs->reg_base.meiw_addr = task->mv_info ? mpp_buffer_get_fd(task->mv_info) : 0;

    regs->reg_base.pic_ofst.pic_ofst_y = mpp_frame_get_offset_y(task->frame);
    regs->reg_base.pic_ofst.pic_ofst_x = mpp_frame_get_offset_x(task->frame);

    setup_vepu580_split(regs, &cfg->split);
    if (prep->width > 1920)
        setup_vepu580_force_slice_split(regs, prep->width);

    setup_vepu580_me(regs, sps, slice);

    vepu580_set_osd(&ctx->osd_cfg);
    setup_vepu580_l2(&ctx->regs_set, slice);
    setup_vepu580_ext_line_buf(regs, ctx);

    mpp_env_get_u32("dump_l1_reg", &dump_l1_reg, 0);

    if (dump_l1_reg) {
        mpp_log("L1 reg dump start:\n");
        RK_U32 *p = (RK_U32 *)regs;
        RK_S32 n = 0x1D0 / sizeof(RK_U32);
        RK_S32 i;

        for (i = 0; i < n; i++)
            mpp_log("%04x %08x\n", i * 4, p[i]);

        mpp_log("L1 reg done\n");
    }

    ctx->frame_cnt++;

    hal_h264e_dbg_func("leave %p\n", hal);
    return MPP_OK;
}

static MPP_RET hal_h264e_vepu580_start(void *hal, HalEncTask *task)
{
    MPP_RET ret = MPP_OK;
    HalH264eVepu580Ctx *ctx = (HalH264eVepu580Ctx *)hal;

    (void) task;

    hal_h264e_dbg_func("enter %p\n", hal);

    do {
        MppDevRegWrCfg wr_cfg;
        MppDevRegRdCfg rd_cfg;

        wr_cfg.reg = &ctx->regs_set.reg_ctl;
        wr_cfg.size = sizeof(ctx->regs_set.reg_ctl);
        wr_cfg.offset = VEPU580_CONTROL_CFG_OFFSET;
#if DUMP_REG
        {
            RK_U32 i;
            RK_U32 *reg = (RK_U32)wr_cfg.reg;
            for ( i = 0; i < sizeof(ctx->regs_set.reg_ctl) / sizeof(RK_U32); i++) {
                /* code */
                mpp_log("reg[%d] = 0x%08x\n", i, reg[i]);
            }

        }
#endif
        ret = mpp_dev_ioctl(ctx->dev, MPP_DEV_REG_WR, &wr_cfg);
        if (ret) {
            mpp_err_f("set register write failed %d\n", ret);
            break;
        }
        wr_cfg.reg = &ctx->regs_set.reg_base;
        wr_cfg.size = sizeof(ctx->regs_set.reg_base);
        wr_cfg.offset = VEPU580_BASE_CFG_OFFSET;

        ret = mpp_dev_ioctl(ctx->dev, MPP_DEV_REG_WR, &wr_cfg);
        if (ret) {
            mpp_err_f("set register write failed %d\n", ret);
            break;
        }
        wr_cfg.reg = &ctx->regs_set.reg_rc_klut;
        wr_cfg.size = sizeof(ctx->regs_set.reg_rc_klut);
        wr_cfg.offset = VEPU580_RC_KLUT_CFG_OFFSET;

        ret = mpp_dev_ioctl(ctx->dev, MPP_DEV_REG_WR, &wr_cfg);
        if (ret) {
            mpp_err_f("set register write failed %d\n", ret);
            break;
        }
        wr_cfg.reg = &ctx->regs_set.reg_s3;
        wr_cfg.size = sizeof(ctx->regs_set.reg_s3);
        wr_cfg.offset = VEPU580_SECTION_3_OFFSET;

        ret = mpp_dev_ioctl(ctx->dev, MPP_DEV_REG_WR, &wr_cfg);
        if (ret) {
            mpp_err_f("set register write failed %d\n", ret);
            break;
        }
        wr_cfg.reg = &ctx->regs_set.reg_rdo;
        wr_cfg.size = sizeof(ctx->regs_set.reg_rdo);
        wr_cfg.offset = VEPU580_RDO_CFG_OFFSET;

        ret = mpp_dev_ioctl(ctx->dev, MPP_DEV_REG_WR, &wr_cfg);
        if (ret) {
            mpp_err_f("set register write failed %d\n", ret);
            break;
        }
        wr_cfg.reg = &ctx->regs_set.reg_osd;
        wr_cfg.size = sizeof(ctx->regs_set.reg_osd);
        wr_cfg.offset = VEPU580_OSD_OFFSET;

        ret = mpp_dev_ioctl(ctx->dev, MPP_DEV_REG_WR, &wr_cfg);
        if (ret) {
            mpp_err_f("set register write failed %d\n", ret);
            break;
        }

        rd_cfg.reg = &ctx->regs_set.reg_st;
        rd_cfg.size = sizeof(ctx->regs_set.reg_st);
        rd_cfg.offset = VEPU580_STATUS_OFFSET;

        ret = mpp_dev_ioctl(ctx->dev, MPP_DEV_REG_RD, &rd_cfg);
        if (ret) {
            mpp_err_f("set register read failed %d\n", ret);
            break;
        }

        /* send request to hardware */
        ret = mpp_dev_ioctl(ctx->dev, MPP_DEV_CMD_SEND, NULL);
        if (ret) {
            mpp_err_f("send cmd failed %d\n", ret);
            break;
        }
    } while (0);

    hal_h264e_dbg_func("leave %p\n", hal);

    return ret;
}

static MPP_RET hal_h264e_vepu580_status_check(void *hal)
{
    HalH264eVepu580Ctx *ctx = (HalH264eVepu580Ctx *)hal;
    HalVepu580RegSet *regs_set = &ctx->regs_set;

    if (regs_set->reg_ctl.int_sta.lkt_node_done_sta)
        hal_h264e_dbg_detail("lkt_done finish");

    if (regs_set->reg_ctl.int_sta.enc_done_sta)
        hal_h264e_dbg_detail("enc_done finish");

    if (regs_set->reg_ctl.int_sta.slc_done_sta)
        hal_h264e_dbg_detail("enc_slice finsh");

    if (regs_set->reg_ctl.int_sta.sclr_done_sta)
        hal_h264e_dbg_detail("safe clear finsh");

    if (regs_set->reg_ctl.int_sta.bsf_oflw_sta)
        mpp_err_f("bit stream overflow");

    if (regs_set->reg_ctl.int_sta.brsp_otsd_sta)
        mpp_err_f("bus write full");

    if (regs_set->reg_ctl.int_sta.wbus_err_sta)
        mpp_err_f("bus write error");

    if (regs_set->reg_ctl.int_sta.rbus_err_sta)
        mpp_err_f("bus read error");

    if (regs_set->reg_ctl.int_sta.wdg_sta)
        mpp_err_f("wdg timeout");

    return MPP_OK;
}

static MPP_RET hal_h264e_vepu580_wait(void *hal, HalEncTask *task)
{
    MPP_RET ret = MPP_OK;
    HalH264eVepu580Ctx *ctx = (HalH264eVepu580Ctx *)hal;

    hal_h264e_dbg_func("enter %p\n", hal);

    ret = mpp_dev_ioctl(ctx->dev, MPP_DEV_CMD_POLL, NULL);
    if (ret) {
        mpp_err_f("poll cmd failed %d\n", ret);
        ret = MPP_ERR_VPUHW;
    } else {
        hal_h264e_vepu580_status_check(hal);
        task->hw_length += ctx->regs_set.reg_st.bs_lgth_l32;
    }

    hal_h264e_dbg_func("leave %p\n", hal);

    return ret;
}

static MPP_RET hal_h264e_vepu580_ret_task(void *hal, HalEncTask *task)
{
    HalH264eVepu580Ctx *ctx = (HalH264eVepu580Ctx *)hal;
    EncRcTaskInfo *rc_info = &task->rc_task->info;
    RK_U32 mb_w = ctx->sps->pic_width_in_mbs;
    RK_U32 mb_h = ctx->sps->pic_height_in_mbs;
    RK_U32 mbs = mb_w * mb_h;

    hal_h264e_dbg_func("enter %p\n", hal);

    // update total hardware length
    task->length += task->hw_length;

    // setup bit length for rate control
    rc_info->bit_real = task->hw_length * 8;
    rc_info->quality_real = ctx->regs_set.reg_st.qp_sum / mbs;
    rc_info->madi = (!ctx->regs_set.reg_st.st_bnum_b16.num_b16) ? 0 :
                    ctx->regs_set.reg_st.madi /  ctx->regs_set.reg_st.st_bnum_b16.num_b16;
    rc_info->madp = (!ctx->regs_set.reg_st.st_bnum_cme.num_ctu) ? 0 :
                    ctx->regs_set.reg_st.madi / ctx->regs_set.reg_st.st_bnum_cme.num_ctu;
    rc_info->iblk4_prop = (ctx->regs_set.reg_st.st_pnum_i4.pnum_i4 +
                           ctx->regs_set.reg_st.st_pnum_i8.pnum_i8 +
                           ctx->regs_set.reg_st.st_pnum_i16.pnum_i16) * 256 / mbs;

    ctx->hal_rc_cfg.bit_real = rc_info->bit_real;
    ctx->hal_rc_cfg.quality_real = rc_info->quality_real;
    ctx->hal_rc_cfg.iblk4_prop = rc_info->iblk4_prop;

    task->hal_ret.data   = &ctx->hal_rc_cfg;
    task->hal_ret.number = 1;

    hal_h264e_dbg_func("leave %p\n", hal);

    return MPP_OK;
}

const MppEncHalApi hal_h264e_vepu580 = {
    .name       = "hal_h264e_vepu580",
    .coding     = MPP_VIDEO_CodingAVC,
    .ctx_size   = sizeof(HalH264eVepu580Ctx),
    .flag       = 0,
    .init       = hal_h264e_vepu580_init,
    .deinit     = hal_h264e_vepu580_deinit,
    .prepare    = hal_h264e_vepu580_prepare,
    .get_task   = hal_h264e_vepu580_get_task,
    .gen_regs   = hal_h264e_vepu580_gen_regs,
    .start      = hal_h264e_vepu580_start,
    .wait       = hal_h264e_vepu580_wait,
    .part_start = NULL,
    .part_wait  = NULL,
    .ret_task   = hal_h264e_vepu580_ret_task,
};