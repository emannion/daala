/*Daala video codec
Copyright (c) 2012 Daala project contributors.  All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

- Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

- Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.*/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdlib.h>
#include <stdio.h>
#include "internal.h"
#include "decint.h"
#include "entdec.h"
#include "entcode.h"
#include "logging.h"
#include "laplace_code.h"
#include "pvq_code.h"

#define OD_PVQ_SKIP_ZERO 1
#define OD_PVQ_SKIP_COPY 2

static void od_decode_pvq_codeword(od_ec_dec *ec, od_adapt_ctx *adapt,
 od_coeff *y, int n, int k, int noref) {
  if (k == 1 && n < 16) {
    int cdf_id;
    int pos;
    cdf_id = 2*(n == 15) + !noref;
    OD_CLEAR(y, n);
    pos = od_decode_cdf_adapt(ec, adapt->pvq_k1_cdf[cdf_id], n - !noref,
     adapt->pvq_k1_increment);
    y[pos] = 1;
    if (od_ec_dec_bits(ec, 1)) y[pos] = -y[pos];
  }
  else {
    int speed = 5;
    int *pvq_adapt;
    int adapt_curr[OD_NSB_ADAPT_CTXS] = { 0 };
    pvq_adapt = adapt->pvq_adapt;
    laplace_decode_vector(ec, y, n - !noref, k, adapt_curr,
     pvq_adapt);
    if (adapt_curr[OD_ADAPT_K_Q8] > 0) {
      pvq_adapt[OD_ADAPT_K_Q8] += (256*adapt_curr[OD_ADAPT_K_Q8]
       - pvq_adapt[OD_ADAPT_K_Q8]) >> speed;
      pvq_adapt[OD_ADAPT_SUM_EX_Q8] += (adapt_curr[OD_ADAPT_SUM_EX_Q8]
       - pvq_adapt[OD_ADAPT_SUM_EX_Q8]) >> speed;
    }
    if (adapt_curr[OD_ADAPT_COUNT_Q8] > 0) {
      pvq_adapt[OD_ADAPT_COUNT_Q8] += (adapt_curr[OD_ADAPT_COUNT_Q8]
       - pvq_adapt[OD_ADAPT_COUNT_Q8]) >> speed;
      pvq_adapt[OD_ADAPT_COUNT_EX_Q8] += (adapt_curr[OD_ADAPT_COUNT_EX_Q8]
       - pvq_adapt[OD_ADAPT_COUNT_EX_Q8]) >> speed;
    }
  }
}

typedef struct {
  od_coeff *ref;
  int nb_coeffs;
  int allow_flip;
} cfl_ctx;

/** Decodes a single vector of integers (eg, a partition within a
 *  coefficient block) encoded using PVQ
 *
 * @param [in,out] ec      range encoder
 * @param [in]     q       scale/quantizer
 * @param [in]     n       number of coefficients in partition
 * @param [in,out] model   entropy decoder state
 * @param [in,out] adapt   adaptation context
 * @param [in,out] exg     ExQ16 expectation of decoded gain value
 * @param [in,out] ext     ExQ16 expectation of decoded theta value
 * @param [in]     ref     'reference' (prediction) vector
 * @param [out]    out     decoded partition
 * @param [in]     noref   boolean indicating absence of reference
 * @param [in]     beta    per-band activity masking beta param
 * @param [in]     robust  stream is robust to error in the reference
 * @param [in]     is_keyframe whether we're encoding a keyframe
 * @param [in]     pli     plane index
 * @param [in]     cdf_ctx selects which cdf context to use
 */
static void pvq_decode_partition(od_ec_dec *ec,
                                 int q0,
                                 int n,
                                 generic_encoder model[3],
                                 od_adapt_ctx *adapt,
                                 int *exg,
                                 int *ext,
                                 od_coeff *ref,
                                 od_coeff *out,
                                 int noref,
                                 double beta,
                                 int robust,
                                 int is_keyframe,
                                 int pli,
                                 int cdf_ctx,
                                 cfl_ctx *cfl,
                                 int has_skip,
                                 int *skip_rest) {
  int k;
  double qcg;
  int max_theta;
  int itheta;
  double theta;
  double gr;
  double gain_offset;
  od_coeff y[1024];
  int qg;
  double q;
  int nodesync;
  int skip;
  int id;
  /* Quantization step calibration to account for the activity masking. */
  q = q0*pow(256<<OD_COEFF_SHIFT, 1./beta - 1);
  theta = 0;
  gr = 0;
  gain_offset = 0;
  itheta = 0;
  /* We always use the robust bitstream for keyframes to avoid having
     PVQ and entropy decoding depending on each other, hurting parallelism. */
  nodesync = robust || is_keyframe;
  /* Jointly decode gain, itheta and noref for small values. Then we handle
     larger gain. We need to wait for itheta because in the !nodesync case
     it depends on max_theta, which depends on the gain. */
  if (*skip_rest) {
    qg = 0;
    if (is_keyframe) {
      itheta = -1;
      noref = 1;
    }
    else {
      itheta = 0;
      noref = 0;
    }
  }
  else {
    id = od_decode_cdf_adapt(ec, &adapt->pvq_gaintheta_cdf[cdf_ctx][0],
     8 + (8 - !is_keyframe)*has_skip, adapt->pvq_gaintheta_increment);
    if (!is_keyframe && id >= 10) id++;
    if (id >= 8) {
      id -= 8;
      *skip_rest = 1;
    }
    qg = id & 1;
    itheta = (id >> 1) - 1;
    noref = (itheta == -1);
  }
  if (qg > 0) {
    int tmp;
    tmp = *exg;
    qg = 1 + generic_decode(ec, &model[!noref], -1, &tmp, 2);
    *exg += (qg << (16 - 2)) - (*exg >> 2);
  }
  skip = 0;
  if(!noref){
    /* we have a reference; compute its gain */
    double cgr;
    int icgr;
    cgr = pvq_compute_gain(ref, n, q, &gr, beta);
    if (pli != 0 && is_keyframe && !OD_DISABLE_CFL) cgr = 1;
    icgr = (int)floor(.5+cgr);
    /* quantized gain is interleave encoded when there's a reference;
       deinterleave it now */
    if (is_keyframe) qg = neg_deinterleave(qg, icgr);
    else {
      qg = neg_deinterleave(qg, icgr + 1) - 1;
      if (qg == 0) skip = (icgr ? OD_PVQ_SKIP_ZERO : OD_PVQ_SKIP_COPY);
    }
    gain_offset = cgr-icgr;
    qcg = qg + gain_offset;
    /* read and decode first-stage PVQ error theta */
    max_theta = pvq_compute_max_theta(qcg, beta);
    if (itheta > 1 && (nodesync || max_theta > 3)) {
      int tmp;
      tmp = *ext;
      itheta = 2 + generic_decode(ec, &model[2], nodesync ? -1 : max_theta - 3,
       &tmp, 2);
      *ext += (itheta << (16 - 2)) - (*ext >> 2);
    }
    theta = pvq_compute_theta(itheta, max_theta);
  }
  else{
    itheta = 0;
    if (!is_keyframe) qg++;
    qcg = qg;
  }

  k = pvq_compute_k(qcg, itheta, theta, noref, n, beta, nodesync);
  if (k != 0) {
    /* when noref==0, y is actually size n-1 */
    od_decode_pvq_codeword(ec, adapt, y, n, k, noref);
  } else {
    OD_CLEAR(y, n);
  }
  /* The CfL flip bit is only decoded on the first band that has noref=0. */
  if (cfl->allow_flip && !noref) {
    int flip;
    int i;
    flip = od_ec_dec_bits(ec, 1);
    if (flip) {
      for (i = 0; i < cfl->nb_coeffs; i++) cfl->ref[i] = -cfl->ref[i];
    }
    cfl->allow_flip = 0;
  }
  pvq_synthesis(out, y, ref, n, gr, noref, qg, gain_offset, theta, q, beta);
  if (skip) {
    int i;
    if (skip == OD_PVQ_SKIP_COPY) for (i = 0; i < n; i++) out[i] = ref[i];
    else for (i = 0; i < n; i++) out[i] = 0;
  }
}

/** Decodes a coefficient block (except for DC) encoded using PVQ
 *
 * @param [in,out] dec     daala decoder context
 * @param [in]     ref     'reference' (prediction) vector
 * @param [out]    out     decoded partition
 * @param [in]     q       quantizer
 * @param [in]     pli     plane index
 * @param [in]     ln      log of the block size minus two
 * @param [in]     qm      per-band quantization matrix
 * @param [in]     beta    per-band activity masking beta param
 * @param [in]     robust  stream is robust to error in the reference
 * @param [in]     is_keyframe whether we're encoding a keyframe
 */
void pvq_decode(daala_dec_ctx *dec,
                od_coeff *ref,
                od_coeff *out,
                int q,
                int pli,
                int ln,
                const int *qm,
                const double *beta,
                int robust,
                int is_keyframe){

  int noref[PVQ_MAX_PARTITIONS];
  int *exg;
  int *ext;
  int nb_bands;
  int i;
  const int *off;
  int size[PVQ_MAX_PARTITIONS];
  generic_encoder *model;
  int skip;
  int skip_rest;
  cfl_ctx cfl;
  exg = &dec->state.adapt.pvq_exg[pli][ln][0];
  ext = dec->state.adapt.pvq_ext + ln*PVQ_MAX_PARTITIONS;
  model = dec->state.adapt.pvq_param_model;
  nb_bands = od_band_offsets[ln][0];
  off = &od_band_offsets[ln][1];
  if (is_keyframe) skip = 0;
  else {
    skip = od_decode_cdf_adapt(&dec->ec, dec->state.adapt.skip_cdf[pli], 4,
     dec->state.adapt.skip_increment);
    out[0] = skip&1;
    skip >>= 1;
  }
  if (skip) {
    for (i = 1; i < 1 << (2*ln + 4); i++) out[i] = ref[i];
  }
  else {
    for (i = 0; i < nb_bands; i++) size[i] = off[i+1] - off[i];
    cfl.ref = ref;
    cfl.nb_coeffs = off[nb_bands];
    cfl.allow_flip = pli != 0 && is_keyframe;
    skip_rest = 0;
    for (i = 0; i < nb_bands; i++) {
      pvq_decode_partition(&dec->ec, OD_MAXI(1, q*qm[i + 1] >> 4), size[i],
       model, &dec->state.adapt, exg + i, ext + i, ref + off[i], out + off[i],
       noref[i], beta[i], robust, is_keyframe, pli, ln*PVQ_MAX_PARTITIONS + i,
       &cfl, i == 0 && (i < nb_bands - 1), &skip_rest);
    }
  }
}
