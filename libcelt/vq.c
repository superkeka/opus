/* (C) 2007-2008 Jean-Marc Valin, CSIRO
*/
/*
   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:
   
   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
   
   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
   
   - Neither the name of the Xiph.org Foundation nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.
   
   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "mathops.h"
#include "cwrs.h"
#include "vq.h"
#include "arch.h"
#include "os_support.h"

/** Takes the pitch vector and the decoded residual vector, computes the gain
    that will give ||p+g*y||=1 and mixes the residual with the pitch. */
static void mix_pitch_and_residual(int * restrict iy, celt_norm_t * restrict X, int N, int K, const celt_norm_t * restrict P)
{
   int i;
   celt_word32_t Ryp, Ryy, Rpp;
   celt_word32_t g;
   VARDECL(celt_norm_t, y);
#ifdef FIXED_POINT
   int yshift;
#endif
   SAVE_STACK;
#ifdef FIXED_POINT
   yshift = 13-celt_ilog2(K);
#endif
   ALLOC(y, N, celt_norm_t);

   /*for (i=0;i<N;i++)
   printf ("%d ", iy[i]);*/
   Rpp = 0;
   i=0;
   do {
      Rpp = MAC16_16(Rpp,P[i],P[i]);
      y[i] = SHL16(iy[i],yshift);
   } while (++i < N);

   Ryp = 0;
   Ryy = 0;
   /* If this doesn't generate a dual MAC (on supported archs), fire the compiler guy */
   i=0;
   do {
      Ryp = MAC16_16(Ryp, y[i], P[i]);
      Ryy = MAC16_16(Ryy, y[i], y[i]);
   } while (++i < N);

   /* g = (sqrt(Ryp^2 + Ryy - Rpp*Ryy)-Ryp)/Ryy */
   g = MULT16_32_Q15(
            celt_sqrt(MULT16_16(ROUND16(Ryp,14),ROUND16(Ryp,14)) + Ryy -
                      MULT16_16(ROUND16(Ryy,14),ROUND16(Rpp,14)))
            - ROUND16(Ryp,14),
       celt_rcp(SHR32(Ryy,9)));

   i=0;
   do 
      X[i] = P[i] + ROUND16(MULT16_16(y[i], g),11);
   while (++i < N);

   RESTORE_STACK;
}


void alg_quant(celt_norm_t *X, celt_mask_t *W, int N, int K, const celt_norm_t *P, ec_enc *enc)
{
   VARDECL(celt_norm_t, y);
   VARDECL(int, iy);
   VARDECL(int, signx);
   int j, is;
   celt_word16_t s;
   int pulsesLeft;
   celt_word32_t sum;
   celt_word32_t xy, yy, yp;
   celt_word16_t Rpp;
   int N_1; /* Inverse of N, in Q14 format (even for float) */
#ifdef FIXED_POINT
   int yshift;
#endif
   SAVE_STACK;

#ifdef FIXED_POINT
   yshift = 13-celt_ilog2(K);
#endif

   ALLOC(y, N, celt_norm_t);
   ALLOC(iy, N, int);
   ALLOC(signx, N, int);
   N_1 = 512/N;

   sum = 0;
   j=0; do {
      X[j] -= P[j];
      if (X[j]>0)
         signx[j]=1;
      else
         signx[j]=-1;
      iy[j] = 0;
      y[j] = 0;
      sum = MAC16_16(sum, P[j],P[j]);
   } while (++j<N);
   Rpp = ROUND16(sum, NORM_SHIFT);

   celt_assert2(Rpp<=NORM_SCALING, "Rpp should never have a norm greater than unity");

   xy = yy = yp = 0;

   pulsesLeft = K;
   while (pulsesLeft > 0)
   {
      int pulsesAtOnce=1;
      int best_id;
      celt_word16_t magnitude;
#ifdef FIXED_POINT
      int rshift;
#endif
      /* Decide on how many pulses to find at once */
      pulsesAtOnce = (pulsesLeft*N_1)>>9; /* pulsesLeft/N */
      if (pulsesAtOnce<1)
         pulsesAtOnce = 1;
#ifdef FIXED_POINT
      rshift = yshift+1+celt_ilog2(K-pulsesLeft+pulsesAtOnce);
#endif
      magnitude = SHL16(pulsesAtOnce, yshift);

      best_id = 0;
      /* The squared magnitude term gets added anyway, so we might as well 
         add it outside the loop */
      yy = ADD32(yy, MULT16_16(magnitude,magnitude));
      /* Choose between fast and accurate strategy depending on where we are in the search */
      if (pulsesLeft>1)
      {
         /* This should ensure that anything we can process will have a better score */
         celt_word32_t best_num = -VERY_LARGE16;
         celt_word16_t best_den = 0;
         j=0;
         do {
            celt_word16_t Rxy, Ryy;
            /* Select sign based on X[j] alone */
            s = signx[j]*magnitude;
            /* Temporary sums of the new pulse(s) */
            Rxy = EXTRACT16(SHR32(xy + MULT16_16(s,X[j]),rshift));
            /* We're multiplying y[j] by two so we don't have to do it here */
            Ryy = EXTRACT16(SHR32(yy + MULT16_16(s,y[j]),rshift));
            
            /* Approximate score: we maximise Rxy/sqrt(Ryy) (we're guaranteed that 
               Rxy is positive because the sign is pre-computed) */
            Rxy = MULT16_16_Q15(Rxy,Rxy);
            /* The idea is to check for num/den >= best_num/best_den, but that way
               we can do it without any division */
            /* OPT: Make sure to use conditional moves here */
            if (MULT16_16(best_den, Rxy) > MULT16_16(Ryy, best_num))
            {
               best_den = Ryy;
               best_num = Rxy;
               best_id = j;
            }
         } while (++j<N);
      } else {
         celt_word16_t g;
         celt_word16_t best_num = -VERY_LARGE16;
         celt_word16_t best_den = 0;
         j=0;
         do {
            celt_word16_t Rxy, Ryy, Ryp;
            celt_word16_t num;
            /* Select sign based on X[j] alone */
            s = signx[j]*magnitude;
            /* Temporary sums of the new pulse(s) */
            Rxy = ROUND16(xy + MULT16_16(s,X[j]), 14);
            /* We're multiplying y[j] by two so we don't have to do it here */
            Ryy = ROUND16(yy + MULT16_16(s,y[j]), 14);
            Ryp = ROUND16(yp + MULT16_16(s,P[j]), 14);

            /* Compute the gain such that ||p + g*y|| = 1 
               ...but instead, we compute g*Ryy to avoid dividing */
            g = celt_psqrt(MULT16_16(Ryp,Ryp) + MULT16_16(Ryy,QCONST16(1.f,14)-Rpp)) - Ryp;
            /* Knowing that gain, what's the error: (x-g*y)^2 
               (result is negated and we discard x^2 because it's constant) */
            /* score = 2*g*Rxy - g*g*Ryy;*/
#ifdef FIXED_POINT
            /* No need to multiply Rxy by 2 because we did it earlier */
            num = MULT16_16_Q15(ADD16(SUB16(Rxy,g),Rxy),g);
#else
            num = g*(2*Rxy-g);
#endif
            if (MULT16_16(best_den, num) > MULT16_16(Ryy, best_num))
            {
               best_den = Ryy;
               best_num = num;
               best_id = j;
            }
         } while (++j<N);
      }
      
      j = best_id;
      is = signx[j]*pulsesAtOnce;
      s = SHL16(is, yshift);

      /* Updating the sums of the new pulse(s) */
      xy = xy + MULT16_16(s,X[j]);
      /* We're multiplying y[j] by two so we don't have to do it here */
      yy = yy + MULT16_16(s,y[j]);
      yp = yp + MULT16_16(s, P[j]);

      /* Only now that we've made the final choice, update y/iy */
      /* Multiplying y[j] by 2 so we don't have to do it everywhere else */
      y[j] += 2*s;
      iy[j] += is;
      pulsesLeft -= pulsesAtOnce;
   }
   
   encode_pulses(iy, N, K, enc);
   
   /* Recompute the gain in one pass to reduce the encoder-decoder mismatch
   due to the recursive computation used in quantisation. */
   mix_pitch_and_residual(iy, X, N, K, P);
   RESTORE_STACK;
}


/** Decode pulse vector and combine the result with the pitch vector to produce
    the final normalised signal in the current band. */
void alg_unquant(celt_norm_t *X, int N, int K, celt_norm_t *P, ec_dec *dec)
{
   VARDECL(int, iy);
   SAVE_STACK;
   ALLOC(iy, N, int);
   decode_pulses(iy, N, K, dec);
   mix_pitch_and_residual(iy, X, N, K, P);
   RESTORE_STACK;
}

void renormalise_vector(celt_norm_t *X, celt_word16_t value, int N, int stride)
{
   int i;
   celt_word32_t E = EPSILON;
   celt_word16_t g;
   celt_norm_t *xptr = X;
   for (i=0;i<N;i++)
   {
      E = MAC16_16(E, *xptr, *xptr);
      xptr += stride;
   }

   g = MULT16_16_Q15(value,celt_rcp(SHL32(celt_sqrt(E),9)));
   xptr = X;
   for (i=0;i<N;i++)
   {
      *xptr = PSHR32(MULT16_16(g, *xptr),8);
      xptr += stride;
   }
}

static void fold(const CELTMode *m, int N, celt_norm_t *Y, celt_norm_t * restrict P, int N0, int B)
{
   int j;
   const int C = CHANNELS(m);
   int id = N0 % (C*B);
   /* Here, we assume that id will never be greater than N0, i.e. that 
      no band is wider than N0. */
   for (j=0;j<C*N;j++)
      P[j] = Y[id++];
}

#define KGAIN 6

void intra_prediction(const CELTMode *m, celt_norm_t * restrict x, celt_mask_t *W, int N, int K, celt_norm_t *Y, celt_norm_t * restrict P, int N0, int B, ec_enc *enc)
{
   int j;
   celt_word16_t s = 1;
   int sign;
   celt_word16_t pred_gain;
   celt_word32_t xy=0;
   const int C = CHANNELS(m);

   pred_gain = celt_div((celt_word32_t)MULT16_16(Q15_ONE,N),(celt_word32_t)(N+KGAIN*K));

   fold(m, N, Y, P, N0, B);

   for (j=0;j<C*N;j++)
      xy = MAC16_16(xy, P[j], x[j]);
   if (xy<0)
   {
      s = -1;
      sign = 1;
   } else {
      s = 1;
      sign = 0;
   }
   ec_enc_bits(enc,sign,1);

   renormalise_vector(P, s*pred_gain, C*N, 1);
}

void intra_unquant(const CELTMode *m, celt_norm_t *x, int N, int K, celt_norm_t *Y, celt_norm_t * restrict P, int N0, int B, ec_dec *dec)
{
   celt_word16_t s;
   celt_word16_t pred_gain;
   const int C = CHANNELS(m);
      
   if (ec_dec_bits(dec, 1) == 0)
      s = 1;
   else
      s = -1;
   
   pred_gain = celt_div((celt_word32_t)MULT16_16(Q15_ONE,N),(celt_word32_t)(N+KGAIN*K));
   
   fold(m, N, Y, P, N0, B);
   
   renormalise_vector(P, s*pred_gain, C*N, 1);
}

void intra_fold(const CELTMode *m, celt_norm_t *x, int N, celt_norm_t *Y, celt_norm_t * restrict P, int N0, int B)
{
   const int C = CHANNELS(m);

   fold(m, N, Y, P, N0, B);
   
   renormalise_vector(P, Q15ONE, C*N, 1);
}

