/********************************************************************
 *                                                                  *
 * THIS FILE IS PART OF THE OggVorbis SOFTWARE CODEC SOURCE CODE.   *
 * USE, DISTRIBUTION AND REPRODUCTION OF THIS LIBRARY SOURCE IS     *
 * GOVERNED BY A BSD-STYLE SOURCE LICENSE INCLUDED WITH THIS SOURCE *
 * IN 'COPYING'. PLEASE READ THESE TERMS BEFORE DISTRIBUTING.       *
 *                                                                  *
 * THE OggVorbis SOURCE CODE IS (C) COPYRIGHT 1994-2001             *
 * by the XIPHOPHORUS Company http://www.xiph.org/                  *
 *                                                                  *
 ********************************************************************

 function: floor backend 1 implementation
 last mod: $Id: floor1.c,v 1.1.2.2 2001/04/29 22:21:04 xiphmont Exp $

 ********************************************************************/

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ogg/ogg.h>
#include "vorbis/codec.h"
#include "codec_internal.h"
#include "registry.h"
#include "codebook.h"
#include "misc.h"
#include "scales.h"

#include <stdio.h>

#define VORBIS_IEEE_FLOAT32

#define floor1_rangedB 140 /* floor 0 fixed at -140dB to 0dB range */


typedef struct {
  int sorted_index[VIF_POSIT+2];  /* en/de */
  int forward_index[VIF_POSIT+2]; /* en/de */
  int reverse_index[VIF_POSIT+2]; /* en */
  
  int hineighbor[VIF_POSIT]; /* de */
  int loneighbor[VIF_POSIT]; /* de */
  int posts;

  int n;
  int quant_q;
  vorbis_info_floor1 *vi;
} vorbis_look_floor1;

typedef struct lsfit_acc{
  long x0;
  long x1;

  long xa;
  long ya;
  long x2a;
  long y2a;
  long xya;
  long n;
} lsfit_acc;

/***********************************************/
 
static vorbis_info_floor *floor1_copy_info (vorbis_info_floor *i){
  vorbis_info_floor1 *info=(vorbis_info_floor1 *)i;
  vorbis_info_floor1 *ret=_ogg_malloc(sizeof(vorbis_info_floor1));
  memcpy(ret,info,sizeof(vorbis_info_floor1));
  return(ret);
}

static void floor1_free_info(vorbis_info_floor *i){
  if(i){
    memset(i,0,sizeof(vorbis_info_floor1));
    _ogg_free(i);
  }
}

static void floor1_free_look(vorbis_look_floor *i){
  vorbis_look_floor1 *look=(vorbis_look_floor1 *)i;
  if(i){
    memset(look,0,sizeof(vorbis_look_floor1));
    free(i);
  }
}

static int ilog(unsigned int v){
  int ret=0;
  while(v){
    ret++;
    v>>=1;
  }
  return(ret);
}

static int ilog2(unsigned int v){
  int ret=0;
  while(v>1){
    ret++;
    v>>=1;
  }
  return(ret);
}

static void floor1_pack (vorbis_info_floor *i,oggpack_buffer *opb){
  vorbis_info_floor1 *info=(vorbis_info_floor1 *)i;
  int j,k;
  int count=0;
  int rangebits;
  int maxposit=info->postlist[1];
  int maxclass=-1;

  /* save out partitions */
  oggpack_write(opb,info->partitions,5); /* only 0 to 31 legal */
  for(j=0;j<info->partitions;j++){
    oggpack_write(opb,info->partitionclass[j],4); /* only 0 to 15 legal */
    if(maxclass<info->partitionclass[j])maxclass=info->partitionclass[j];
  }

  /* save out partition classes */
  for(j=0;j<maxclass+1;j++){
    oggpack_write(opb,info->class_dim[j]-1,3); /* 1 to 8 */
    oggpack_write(opb,info->class_subs[j],2); /* 0 to 3 */
    if(info->class_subs[j])oggpack_write(opb,info->class_book[j],8);
    for(k=0;k<info->class_subs[j];k++)
      oggpack_write(opb,info->class_subbook[j][k]+1,8);
  }

  /* save out the post list */
  oggpack_write(opb,info->mult-1,2);     /* only 1,2,3,4 legal now */ 
  oggpack_write(opb,ilog2(maxposit),4);
  rangebits=ilog2(maxposit);

  for(j=0,k=0;j<info->partitions;j++){
    count+=info->class_dim[info->partitionclass[j]]; 
    for(;k<count;k++)
      oggpack_write(opb,info->postlist[k+2],rangebits);
  }
}


static vorbis_info_floor *floor1_unpack (vorbis_info *vi,oggpack_buffer *opb){
  codec_setup_info     *ci=vi->codec_setup;
  int j,k,count,maxclass=-1,rangebits;

  vorbis_info_floor1 *info=_ogg_calloc(1,sizeof(vorbis_info_floor1));
  /* read partitions */
  info->partitions=oggpack_read(opb,5); /* only 0 to 31 legal */
  for(j=0;j<info->partitions;j++){
    info->partitionclass[j]=oggpack_read(opb,4); /* only 0 to 15 legal */
    if(maxclass<info->partitionclass[j])maxclass=info->partitionclass[j];
  }

  /* read partition classes */
  for(j=0;j<maxclass+1;j++){
    info->class_dim[j]=oggpack_read(opb,3)+1; /* 1 to 8 */
    info->class_subs[j]=oggpack_read(opb,2); /* 0,1,2,3 bits */
    if(info->class_subs[j]<0)
      goto err_out;
    if(info->class_subs[j])info->class_book[j]=oggpack_read(opb,8);
    if(info->class_book[j]<0 || info->class_book[j]>=ci->books)
      goto err_out;
    for(k=0;k<info->class_subs[j];k++){
      info->class_subbook[j][k]=oggpack_read(opb,8)-1;
      if(info->class_subbook[j][k]<-1 || info->class_subbook[j][k]>=ci->books)
	goto err_out;
    }
  }

  /* read the post list */
  info->mult=oggpack_read(opb,2)+1;     /* only 1,2,3,4 legal now */ 
  rangebits=oggpack_read(opb,4);

  for(j=0,k=0;j<info->partitions;j++){
    count+=info->class_dim[info->partitionclass[j]]; 
    for(;k<count;k++){
      int t=info->postlist[k+2]=oggpack_read(opb,rangebits);
      if(t<0 || t>=(1<<rangebits))
	goto err_out;
    }
  }
  info->postlist[0]=0;
  info->postlist[1]=1<<rangebits;

  return(info);
  
 err_out:
  floor1_free_info(info);
  return(NULL);
}

static int icomp(const void *a,const void *b){
  return(**(int **)a-**(int **)b);
}

static vorbis_look_floor *floor1_look(vorbis_dsp_state *vd,vorbis_info_mode *mi,
                              vorbis_info_floor *in){

  int *sortpointer[VIF_POSIT+2];
  vorbis_info_floor1 *info=(vorbis_info_floor1 *)in;
  vorbis_look_floor1 *look=_ogg_calloc(1,sizeof(vorbis_look_floor1));
  int i,j,n=0;

  look->vi=info;
  look->n=info->postlist[1];
 
  /* we drop each position value in-between already decoded values,
     and use linear interpolation to predict each new value past the
     edges.  The positions are read in the order of the position
     list... we precompute the bounding positions in the lookup.  Of
     course, the neighbors can change (if a position is declined), but
     this is an initial mapping */

  for(i=0;i<info->partitions;i++)n+=info->class_dim[info->partitionclass[i]];
  n+=2;
  look->posts=n;

  /* also store a sorted position index */
  for(i=0;i<n;i++)sortpointer[i]=info->postlist+i;
  qsort(sortpointer,n,sizeof(int),icomp);

  /* points from sort order back to range number */
  for(i=0;i<n;i++)look->forward_index[i]=sortpointer[i]-info->postlist;
  /* points from range order to sorted position */
  for(i=0;i<n;i++)look->reverse_index[look->forward_index[i]]=i;
  /* we actually need the post values too */
  for(i=0;i<n;i++)look->sorted_index[i]=info->postlist[look->forward_index[i]];
  
  /* quantize values to multiplier spec */
  switch(info->mult){
  case 1: /* 1024 -> 256 */
    look->quant_q=256;
    break;
  case 2: /* 1024 -> 128 */
    look->quant_q=128;
    break;
  case 3: /* 1024 -> 86 */
    look->quant_q=86;
    break;
  case 4: /* 1024 -> 64 */
    look->quant_q=64;
    break;
  }

  /* discover our neighbors for decode where we don't use fit flags
     (that would push the neighbors outward) */
  for(i=0;i<n-2;i++){
    int lo=0;
    int hi=1;
    int lx=0;
    int hx=look->n;
    int currentx=info->postlist[i+2];
    for(j=0;j<i+2;j++){
      int x=info->postlist[j];
      if(x>lx && x<currentx){
	lo=j;
	lx=x;
      }
      if(x<hx && x>currentx){
	hi=j;
	hx=x;
      }
    }
    look->loneighbor[i]=lo;
    look->hineighbor[i]=hi;
  }

  return(look);
}

static int render_point(int x0,int x1,int y0,int y1,int x){
  int dy=y1-y0;
  int adx=x1-x0;
  int ady=abs(dy);
  int err=ady*(x-x0);
  
  int off=err/adx;
  if(dy<0)return(y0-off);
  return(y0+off);
}

static int FLOOR_todB_LOOKUP[560]={
	1023, 1021, 1019, 1018, 1016, 1014, 1012, 1010, 
	1008, 1007, 1005, 1003, 1001, 999, 997, 996, 
	994, 992, 990, 988, 986, 985, 983, 981, 
	979, 977, 975, 974, 972, 970, 968, 966, 
	964, 963, 961, 959, 957, 955, 954, 952, 
	950, 948, 946, 944, 943, 941, 939, 937, 
	935, 933, 932, 930, 928, 926, 924, 922, 
	921, 919, 917, 915, 913, 911, 910, 908, 
	906, 904, 902, 900, 899, 897, 895, 893, 
	891, 890, 888, 886, 884, 882, 880, 879, 
	877, 875, 873, 871, 869, 868, 866, 864, 
	862, 860, 858, 857, 855, 853, 851, 849, 
	847, 846, 844, 842, 840, 838, 836, 835, 
	833, 831, 829, 827, 826, 824, 822, 820, 
	818, 816, 815, 813, 811, 809, 807, 805, 
	804, 802, 800, 798, 796, 794, 793, 791, 
	789, 787, 785, 783, 782, 780, 778, 776, 
	774, 772, 771, 769, 767, 765, 763, 762, 
	760, 758, 756, 754, 752, 751, 749, 747, 
	745, 743, 741, 740, 738, 736, 734, 732, 
	730, 729, 727, 725, 723, 721, 719, 718, 
	716, 714, 712, 710, 708, 707, 705, 703, 
	701, 699, 698, 696, 694, 692, 690, 688, 
	687, 685, 683, 681, 679, 677, 676, 674, 
	672, 670, 668, 666, 665, 663, 661, 659, 
	657, 655, 654, 652, 650, 648, 646, 644, 
	643, 641, 639, 637, 635, 634, 632, 630, 
	628, 626, 624, 623, 621, 619, 617, 615, 
	613, 612, 610, 608, 606, 604, 602, 601, 
	599, 597, 595, 593, 591, 590, 588, 586, 
	584, 582, 580, 579, 577, 575, 573, 571, 
	570, 568, 566, 564, 562, 560, 559, 557, 
	555, 553, 551, 549, 548, 546, 544, 542, 
	540, 538, 537, 535, 533, 531, 529, 527, 
	526, 524, 522, 520, 518, 516, 515, 513, 
	511, 509, 507, 506, 504, 502, 500, 498, 
	496, 495, 493, 491, 489, 487, 485, 484, 
	482, 480, 478, 476, 474, 473, 471, 469, 
	467, 465, 463, 462, 460, 458, 456, 454, 
	452, 451, 449, 447, 445, 443, 442, 440, 
	438, 436, 434, 432, 431, 429, 427, 425, 
	423, 421, 420, 418, 416, 414, 412, 410, 
	409, 407, 405, 403, 401, 399, 398, 396, 
	394, 392, 390, 388, 387, 385, 383, 381, 
	379, 378, 376, 374, 372, 370, 368, 367, 
	365, 363, 361, 359, 357, 356, 354, 352, 
	350, 348, 346, 345, 343, 341, 339, 337, 
	335, 334, 332, 330, 328, 326, 324, 323, 
	321, 319, 317, 315, 314, 312, 310, 308, 
	306, 304, 303, 301, 299, 297, 295, 293, 
	292, 290, 288, 286, 284, 282, 281, 279, 
	277, 275, 273, 271, 270, 268, 266, 264, 
	262, 260, 259, 257, 255, 253, 251, 250, 
	248, 246, 244, 242, 240, 239, 237, 235, 
	233, 231, 229, 228, 226, 224, 222, 220, 
	218, 217, 215, 213, 211, 209, 207, 206, 
	204, 202, 200, 198, 196, 195, 193, 191, 
	189, 187, 186, 184, 182, 180, 178, 176, 
	175, 173, 171, 169, 167, 165, 164, 162, 
	160, 158, 156, 154, 153, 151, 149, 147, 
	145, 143, 142, 140, 138, 136, 134, 132, 
	131, 129, 127, 125, 123, 122, 120, 118, 
	116, 114, 112, 111, 109, 107, 105, 103, 
	101, 100, 98, 96, 94, 92, 90, 89, 
	87, 85, 83, 81, 79, 78, 76, 74, 
	72, 70, 68, 67, 65, 63, 61, 59, 
	58, 56, 54, 52, 50, 48, 47, 45, 
	43, 41, 39, 37, 36, 34, 32, 30, 
	28, 26, 25, 23, 21, 19, 17, 15, 
	14, 12, 10, 8, 6, 4, 3, 1, 
};

static float FLOOR_fromdB_LOOKUP[256]={
	1.0649863e-07F, 1.1341951e-07F, 1.2079015e-07F, 1.2863978e-07F, 
	1.3699951e-07F, 1.4590251e-07F, 1.5538408e-07F, 1.6548181e-07F, 
	1.7623575e-07F, 1.8768855e-07F, 1.9988561e-07F, 2.128753e-07F, 
	2.2670913e-07F, 2.4144197e-07F, 2.5713223e-07F, 2.7384213e-07F, 
	2.9163793e-07F, 3.1059021e-07F, 3.3077411e-07F, 3.5226968e-07F, 
	3.7516214e-07F, 3.9954229e-07F, 4.2550680e-07F, 4.5315863e-07F, 
	4.8260743e-07F, 5.1396998e-07F, 5.4737065e-07F, 5.8294187e-07F, 
	6.2082472e-07F, 6.6116941e-07F, 7.0413592e-07F, 7.4989464e-07F, 
	7.9862701e-07F, 8.5052630e-07F, 9.0579828e-07F, 9.6466216e-07F, 
	1.0273513e-06F, 1.0941144e-06F, 1.1652161e-06F, 1.2409384e-06F, 
	1.3215816e-06F, 1.4074654e-06F, 1.4989305e-06F, 1.5963394e-06F, 
	1.7000785e-06F, 1.8105592e-06F, 1.9282195e-06F, 2.0535261e-06F, 
	2.1869758e-06F, 2.3290978e-06F, 2.4804557e-06F, 2.6416497e-06F, 
	2.8133190e-06F, 2.9961443e-06F, 3.1908506e-06F, 3.3982101e-06F, 
	3.6190449e-06F, 3.8542308e-06F, 4.1047004e-06F, 4.3714470e-06F, 
	4.6555282e-06F, 4.9580707e-06F, 5.2802740e-06F, 5.6234160e-06F, 
	5.9888572e-06F, 6.3780469e-06F, 6.7925283e-06F, 7.2339451e-06F, 
	7.7040476e-06F, 8.2047000e-06F, 8.7378876e-06F, 9.3057248e-06F, 
	9.9104632e-06F, 1.0554501e-05F, 1.1240392e-05F, 1.1970856e-05F, 
	1.2748789e-05F, 1.3577278e-05F, 1.4459606e-05F, 1.5399272e-05F, 
	1.6400004e-05F, 1.7465768e-05F, 1.8600792e-05F, 1.9809576e-05F, 
	2.1096914e-05F, 2.2467911e-05F, 2.3928002e-05F, 2.5482978e-05F, 
	2.7139006e-05F, 2.8902651e-05F, 3.0780908e-05F, 3.2781225e-05F, 
	3.4911534e-05F, 3.7180282e-05F, 3.9596466e-05F, 4.2169667e-05F, 
	4.4910090e-05F, 4.7828601e-05F, 5.0936773e-05F, 5.4246931e-05F, 
	5.7772202e-05F, 6.1526565e-05F, 6.5524908e-05F, 6.9783085e-05F, 
	7.4317983e-05F, 7.9147585e-05F, 8.4291040e-05F, 8.9768747e-05F, 
	9.5602426e-05F, 0.00010181521F, 0.00010843174F, 0.00011547824F, 
	0.00012298267F, 0.00013097477F, 0.00013948625F, 0.00014855085F, 
	0.00015820453F, 0.00016848555F, 0.00017943469F, 0.00019109536F, 
	0.00020351382F, 0.00021673929F, 0.00023082423F, 0.00024582449F, 
	0.00026179955F, 0.00027881276F, 0.00029693158F, 0.00031622787F, 
	0.00033677814F, 0.00035866388F, 0.00038197188F, 0.00040679456F, 
	0.00043323036F, 0.00046138411F, 0.00049136745F, 0.00052329927F, 
	0.00055730621F, 0.00059352311F, 0.00063209358F, 0.00067317058F, 
	0.00071691700F, 0.00076350630F, 0.00081312324F, 0.00086596457F, 
	0.00092223983F, 0.00098217216F, 0.0010459992F, 0.0011139742F, 
	0.0011863665F, 0.0012634633F, 0.0013455702F, 0.0014330129F, 
	0.0015261382F, 0.0016253153F, 0.0017309374F, 0.0018434235F, 
	0.0019632195F, 0.0020908006F, 0.0022266726F, 0.0023713743F, 
	0.0025254795F, 0.0026895994F, 0.0028643847F, 0.0030505286F, 
	0.0032487691F, 0.0034598925F, 0.0036847358F, 0.0039241906F, 
	0.0041792066F, 0.0044507950F, 0.0047400328F, 0.0050480668F, 
	0.0053761186F, 0.0057254891F, 0.0060975636F, 0.0064938176F, 
	0.0069158225F, 0.0073652516F, 0.0078438871F, 0.0083536271F, 
	0.0088964928F, 0.009474637F, 0.010090352F, 0.010746080F, 
	0.011444421F, 0.012188144F, 0.012980198F, 0.013823725F, 
	0.014722068F, 0.015678791F, 0.016697687F, 0.017782797F, 
	0.018938423F, 0.020169149F, 0.021479854F, 0.022875735F, 
	0.024362330F, 0.025945531F, 0.027631618F, 0.029427276F, 
	0.031339626F, 0.033376252F, 0.035545228F, 0.037855157F, 
	0.040315199F, 0.042935108F, 0.045725273F, 0.048696758F, 
	0.051861348F, 0.055231591F, 0.058820850F, 0.062643361F, 
	0.066714279F, 0.071049749F, 0.075666962F, 0.080584227F, 
	0.085821044F, 0.091398179F, 0.097337747F, 0.10366330F, 
	0.11039993F, 0.11757434F, 0.12521498F, 0.13335215F, 
	0.14201813F, 0.15124727F, 0.16107617F, 0.17154380F, 
	0.18269168F, 0.19456402F, 0.20720788F, 0.22067342F, 
	0.23501402F, 0.25028656F, 0.26655159F, 0.28387361F, 
	0.30232132F, 0.32196786F, 0.34289114F, 0.36517414F, 
	0.38890521F, 0.41417847F, 0.44109412F, 0.46975890F, 
	0.50028648F, 0.53279791F, 0.56742212F, 0.60429640F, 
	0.64356699F, 0.68538959F, 0.72993007F, 0.77736504F, 
	0.82788260F, 0.88168307F, 0.9389798F, 1.F, 
};

#ifdef VORBIS_IEEE_FLOAT32
static int vorbis_floor1_dBquant(float *x){
  float temp=*x-256.f;
  ogg_uint32_t *i=(ogg_uint32_t *)(&temp);
  if(*i<(ogg_uint32_t)0xc3800000)return(1023);
  if(*i>(ogg_uint32_t)0xc3c5e000)return(0);
  return FLOOR_quantdB_LOOKUP[(*i-0xc3800000)>>13];
}
#else
static int vorbis_floor1_dBquant(float *x){
  int i= ((*x)+140.)/140.*1024.+.5;
  if(i>1023)return(1023);
  if(i<0)return(0);
  return i;
}
#endif

static void render_line(int x0,int x1,int y0,int y1,float *d){
  int dy=y1-y0;
  int adx=x1-x0;
  int ady=abs(dy);
  int base=dy/adx;
  int sy=(dy<0?base-1:base+1);
  int x=x0;
  int y=y0;
  int err=0;

  ady-=abs(base*adx);

  d[x]=FLOOR_fromdB_LOOKUP[y];
  while(++x<x1){
    err=err+ady;
    if(err>=adx){
      err-=adx;
      y+=sy;
    }else{
      y+=base;
    }
    d[x]=FLOOR_fromdB_LOOKUP[y];
  }
}

/* the floor has already been filtered to only include relevant sections */
static int accumulate_fit(float *floor,int x0, int x1,lsfit_acc *a){
  long i;

  memset(a,0,sizeof(lsfit_acc));
  a->x0=x0;
  a->x1=x1;

  for(i=x0;i<x1;i++){
    int quantized=vorbis_floor1_dBquant(floor+i);
    if(quantized){
      a->xa  += i;
      a->ya  += quantized;
      a->x2a += (i*i);
      a->y2a += quantized*quantized;
      a->xya += i*quantized;
      a->n++;
    }
  }
  return(a->n);
}

/* returns < 0 on too few points to fit, >=0 (meansq error) on success */
static int fit_line(lsfit_acc *a,int fits,int *y0,int *y1){
  long x=0,y=0,x2=0,y2=0,xy=0,n=0,i;
  long x0=a[0].x0;
  long x1=a[fits-1].x1;

  if(*y0>=0){  /* hint used to break degenerate cases */
    x+=   x0;
    y+=  *y0;
    x2+=  x0 *  x0;
    y2+= *y0 * *y0;
    xy+= *y0 *  x0;
    n++;
  }

  if(*y1>=0){  /* hint used to break degenerate cases */
    x+=   x1;
    y+=  *y1;
    x2+=  x1 *  x1;
    y2+= *y1 * *y1;
    xy+= *y1 *  x1;
    n++;
  }

  for(i=0;i<fits;i++){
    x+=a[i].xa;
    y+=a[i].ya;
    x2+=a[i].x2a;
    y2+=a[i].y2a;
    xy+=a[i].xya;
    n+=a[i].n;
  }
  if(n<2)return(-1);
  
  {
    /* need 64 bit multiplies, which C doesn't give portably as int */
    double fx=x;
    double fy=y;
    double fx2=x2;
    double fy2=y2;
    double fxy=xy;
    double a=(fy*fx2-fxy*fx)/(n*fx2-fx*fx);
    double b=(n*fxy-fx*fy)/(n*fx2-fx*fx);
    int s=rint((a*a*n + a*b*(2*fx) - a*(2*fy) + b*b*fx2 - b*(2*fxy) + fy2)/(n*n));
    if(s<0)s=0;
    *y0=rint(a+b*x0);
    *y1=rint(a+b*x1);
    return(s);
  }
}

static int inspect_error(int x0,int x1,int y0,int y1,float *flr,
			 vorbis_info_floor1 *info){
  int dy=y1-y0;
  int adx=x1-x0;
  int ady=abs(dy);
  int base=dy/adx;
  int sy=(dy<0?base-1:base+1);
  int x=x0;
  int y=y0;
  int err=0;
  int mse=0;
  int val=vorbis_floor1_dBquant(flr+x);
  int n=0;

  ady-=abs(base*adx);
  
  if(val){
    if(y-info->maxover>val)return(1);
    if(y+info->maxunder<val)return(1);
    mse=(y-val);
    mse*=mse;
    n++;
  }

  while(++x<x1){
    err=err+ady;
    if(err>=adx){
      err-=adx;
      y+=sy;
    }else{
      y+=base;
    }

    val=vorbis_floor1_dBquant(flr+x);
    if(val){
      if(y-info->maxover>val)return(1);
      if(y+info->maxunder<val)return(1);
      mse+=((y-val)*(y-val));
      n++;
    }
  }
  
  if(mse/n>info->maxerr)return(1);
  return(0);
}

int post_Y(int *A,int *B,int pos){
  if(A[pos]==-1)
    return B[pos];
  if(B[pos]==-1)
    return A[pos];
  return (A[pos]+B[pos])>>1;
}

/* didn't need in->out seperation, modifies the flr[] vector; takes in
   a dB scale floor, puts out linear */
static int floor1_forward(vorbis_block *vb,vorbis_look_floor *in,
		    float *flr){
  long i,j,k,l;
  vorbis_look_floor1 *look=(vorbis_look_floor1 *)in;
  vorbis_info_floor1 *info=look->vi;
  long n=look->n;
  long posts=look->posts;
  long nonzero=0;
  lsfit_acc fits[VIF_POSIT+1];
  int fit_valueA[VIF_POSIT+2]; /* index by range list position */
  int fit_valueB[VIF_POSIT+2]; /* index by range list position */
  int fit_flag[VIF_POSIT+2];

  int loneighbor[VIF_POSIT+2]; /* sorted index of range list position (+2) */
  int hineighbor[VIF_POSIT+2]; 
  int memo[VIF_POSIT+2];
  static_codebook *sbooks=vb->vd->vi->codec_setup->book_param;
  codebook *books=vb->vd->backend_state->fullbooks;   

  memset(fit_flag,0,sizeof(fit_flag));
  for(i=0;i<posts;i++)loneighbor[i]=0; /* 0 for the implicit 0 post */
  for(i=0;i<posts;i++)hineighbor[i]=1; /* 1 for the implicit post at n */
  for(i=0;i<posts;i++)memo[i]=-1;      /* no neighbor yet */

  /* quantize the relevant floor points and collect them into line fit
     structures (one per minimal division) at the same time */
  if(posts==0){
    nonzero+=accumulate_fit(flr,0,n,fits);
  }else{
    for(i=0;i<posts-1;i++)
      nonzero+=accumulate_fit(flr,look->sorted_index[i],
			      look->sorted_index[i+1],fits+i);
  }
  
  if(nonzero){
    /* start by fitting the implicit base case.... */
    int y0=-999;
    int y1=-999;
    int mse=fit_line(fits,posts-1,&y0,&y1);
    if(mse<0){
      /* Only a single nonzero point */
      y0=-999;
      y1=0;
      fit_line(fits,posts-1,&y0,&y1);
    }

    fit_flag[0]=1;
    fit_flag[1]=1;
    fit_valueA[0]=y0;
    fit_valueB[0]=y0;
    fit_valueB[1]=y1;
    fit_valueA[1]=y1;

    if(mse>=0){
      /* Non degenerate case */
      /* start progressive splitting.  This is a greedy, non-optimal
	 algorithm, but simple and close enough to the best
	 answer. */
      for(i=2;i<posts;i++){
	int sortpos=look->reverse_index[i];
	int ln=loneighbor[sortpos];
	int hn=hineighbor[sortpos];

	/* eliminate repeat searches of a particular range with a memo */
	if(memo[ln]!=hn){
	  /* haven't performed this error search yet */
	  int lsortpos=look->reverse_index[ln];
	  int hsortpos=look->reverse_index[hn];

	  /* if this is an empty segment, its endpoints don't matter.
	     Mark as such */
	  for(j=lsortpos;j<hsortpos;j++)
	    if(fits[j].n)break;
	  if(j==hsortpos){
	    /* empty segment; important to note that this does not
               break 0/n post case */
	    fit_valueB[ln]=-1;
	    fit_valueA[hn]=-1;
	  }else{
	    /* A note: we want to bound/minimize *local*, not global, error */
	    int lx=info->postlist[ln];
	    int hx=info->postlist[hn];	  
	    int ly=post_Y(fit_valueA,fit_valueB,ln);
	    int hy=post_Y(fit_valueA,fit_valueB,hn);
	    memo[ln]=hn;
	    
	    if(i<info->searchstart ||
	       inspect_error(lx,hx,ly,hy,flr,info)){
	      /* outside error bounds/begin search area.  Split it. */
	      int ly0=-999;
	      int ly1=-999;
	      int hy0=-999;
	      int hy1=-999;
	      int lmse=fit_line(fits+lsortpos,sortpos-lsortpos,&ly0,&ly1);
	      int hmse=fit_line(fits+sortpos,hsortpos-sortpos,&hy0,&hy1);
	      
	      /* Handle degeneracy */
	      if(lmse<0 && hmse<0){
		ly0=fit_valueA[ln];
		hy1=fit_valueB[hn];
		lmse=fit_line(fits+lsortpos,sortpos-lsortpos,&ly0,&ly1);
		hmse=fit_line(fits+sortpos,hsortpos-sortpos,&hy0,&hy1);
	      }

	      if(lmse<0 && hmse<0) continue;
	      
	      if(lmse<0){
		ly0=fit_valueA[ln];
		ly1=hy0;
		lmse=fit_line(fits+lsortpos,sortpos-lsortpos,&ly0,&ly1);
	      }
	      if(hmse<0){
		hy1=fit_valueB[hn];
		hy0=ly1;
		hmse=fit_line(fits+sortpos,hsortpos-sortpos,&hy0,&hy1);
	      }
	      
	      /* store new edge values */
	      fit_valueB[ln]=ly0;
	      if(ln==0)fit_valueA[ln]=ly0;
	      fit_valueA[i]=ly1;
	      fit_valueB[i]=hy0;
	      fit_valueA[hn]=hy1;
	      if(hn==1)fit_valueB[hn]=hy1;
	      
	      /* store new neighbor values */
	      for(j=sortpos-1;j>=0;j--)
		if(hineighbor[j]==hn)
		  hineighbor[j]=i;
		else
		  break;
	      for(j=sortpos+1;j<posts;j++)
		if(loneighbor[j]==ln)
		  loneighbor[j]=i;
		else
		break;
	      
	      /* store flag (set) */
	      fit_flag[i]=1;
	      
	    }
	  }
	}
      }
    }
    
    /* quantize values to multiplier spec */
    switch(info->mult){
    case 1: /* 1024 -> 256 */
      for(i=0;i<posts;i++)
	if(fit_flag[i])
	  fit_valueA[i]=(post_Y(fit_valueA,fit_valueB,i)+2)>>2;
      break;
    case 2: /* 1024 -> 128 */
      for(i=0;i<posts;i++)
	if(fit_flag[i])
	  fit_valueA[i]=(post_Y(fit_valueA,fit_valueB,i)+4)>>3;
      break;
    case 3: /* 1024 -> 86 */
      for(i=0;i<posts;i++)
	if(fit_flag[i])
	  fit_valueA[i]=(post_Y(fit_valueA,fit_valueB,i)+6)/12;
      break;
    case 4: /* 1024 -> 64 */
      for(i=0;i<posts;i++)
	if(fit_flag[i])
	  fit_valueA[i]=(post_Y(fit_valueA,fit_valueB,i)+8)>>4;
      break;
    }

    /* find prediction values for each post and subtract them */
    /* work backward to avoid a copy; unwind the neighbor arrays */
    for(i=posts-1;i>1;i--){
      int sp=look->reverse_index[i];
      int ln=loneighbor[i];
      int hn=hineighbor[i];
      
      int x0=info->postlist[ln];
      int x1=info->postlist[hn];
      int y0=fit_valueA[ln];
      int y1=fit_valueA[hn];
      
      int predicted=render_point(x0,x1,y0,y1,info->postlist[i]);

      if(fit_flag[i]){
	int headroom=(look->quant_q-predicted<predicted?
		      look->quant_q-predicted:predicted);

	int val=fit_valueA[i]-predicted;

	/* at this point the 'deviation' value is in the range +/- max
	   range, but the real, unique range can be mapped to only
	   [0-maxrange).  So we want to wrap the deviation into this
	   limited range, but do it in the way that least screws an
	   essentially gaussian probability distribution. */
	
	if(val<0)
	  if(val<-headroom)
	    val=headroom-val-1;
	  else
	    val=1-(val<<1);
	else
	  if(val>=headroom)
	    val= val+headroom;
	  else
	    val>>=1;

	fit_valueB[i]=val;

	/* unroll the neighbor arrays */
	for(j=sp+1;j<posts;j++)
	  if(loneighbor[j]==i)
	    loneighbor[j]=loneighbor[sp];
	  else
	    break;
	for(j=sp-1;j>=0;j--)
	  if(hineighbor[j]==i)
	    hineighbor[j]=hineighbor[sp];
	  else
	    break;
	
      }else{
	fit_valueA[i]=predicted;
	fit_valueB[i]=0;
      }
    }

    /* we have everything we need. pack it out */
    /* mark nontrivial floor */
    oggpack_write(&vb->opb,1,1);

    /* beginning/end post */
    oggpack_write(&vb->opb,fit_valueA[0],ilog(look->quant_q-1));
    oggpack_write(&vb->opb,fit_valueA[1],ilog(look->quant_q-1));
    
    /* partition by partition */
    for(i=0,j=2;i<info->partitions;i++){
      int class=info->partitionclass[i];
      int cdim=info->class_dim[class];
      int csubbits=info->class_subs[class];
      int csub=1<<csubbits;
      int bookas[8]={0,0,0,0,0,0,0,0};
      int cval=0;
      int cshift=0;

      /* generate the partition's first stage cascade value */
      if(csubbits){
	int maxval[8];
	for(k=0;k<csub;k++){
	  int booknum=info->class_subbook[class][k];
	  if(booknum<0){
	    maxval[k]=0;
	  }else{
	    maxval[k]=sbooks[info->class_subbook[class][k]].entries;
	  }
	}
	for(k=0;k<cdim;k++){
	  for(l=0;l<csub;l++){
	    val=fit_valueB[j+k];
	    if(val<maxval[k]){
	      bookas[k]=l;
	      break;
	    }
	  }
	  cval|= bookas[k]<<cshift;
	  cshift+=csubbits;
	}
	/* write it */
	vorbis_book_encode(books+info->class_book[class],cval,&vb->opb);

#ifdef TRAIN_FLOOR1
	{
	  FILE *of;
	  char buffer[80];
	  sprintf(buffer,"floor1_class%d.vqd",class);
	  of=fopen(buffer,"a");
	  fprintf(of,"%d\n",cval);
	  fclose(of);
	}
#endif
      }
      
      /* write post values */
      for(k=0;k<cdim;k++){
	int book=info->class_subbook[class][bookas[k]];
	if(book>=0){
	  vorbis_book_encode(books+book,
			     fit_valueB[j+k],&vb->opb);
#ifdef TRAIN_FLOOR1
	  {
	    FILE *of;
	    char buffer[80];
	    sprintf(buffer,"floor1_class%dsub%d.vqd",class,bookas[k]);
	    of=fopen(buffer,"a");
	    fprintf(of,"%d\n",fit_valueB[j+k]);
	    fclose(of);
	  }
#endif
	}
      }
      j+=cdim;
    }
    
    /* generate quantized floor equivalent to what we'd unpack in decode */
    {
      int hx;
      int lx=0;
      int ly=fit_valueA[0]*info->mult;
      for(j=1;j<posts;j++){
	int current=look->forward_index[j];
	int hy=fit_valueA[current]*info->mult;
	hx=info->postlist[current];
	
	render_line(lx,hx,ly,hy,out);
	
	lx=hx;
	ly=hy;
      }
      for(j=hx;j<n;j++)out[j]=0.f; /* be certain */
    }    

  }else{
    oggpack_write(&vb->opb,0,1);
    memset(flr,0,n*sizeof(float));
  }
  return(nonzero);
}

static int floor1_inverse(vorbis_block *vb,vorbis_look_floor *i,float *out){
  vorbis_look_floor1 *look=(vorbis_look_floor1 *)i;
  vorbis_info_floor1 *info=look->vi;

  codec_setup_info   *ci=vb->vd->vi->codec_setup;
  int                  n=ci->blocksizes[vb->mode]/2;
  int i,j;
  codebook *books=vb->vd->backend_state->fullbooks;   

  /* unpack wrapped/predicted values from stream */
  if(oggpack_read(&vb->opb,1)==1){
    int fit_value[VIF_POSIT+2];

    fit_value[0]=oggpack_read(&vb->opb,ilog(look->quant_q-1));
    fit_value[1]=oggpack_read(&vb->opb,ilog(look->quant_q-1));

    /* partition by partition */
    /* partition by partition */
    for(i=0,j=2;i<info->partitions;i++){
      int class=info->partitionclass[i];
      int cdim=info->class_dim[class];
      int csubbits=info->class_subs[class];
      int csub=1<<csubbits;
      int bookas[8]={0,0,0,0,0,0,0,0};

      /* decode the partition's first stage cascade value */
      if(csubbits){
	int cval=vorbis_book_decode(books+info->class_book[class],&vb->opb);
	if(cval==-1)goto eop;

	for(k=0;k<cdim;k++){
	  int book=info->class_subbook[class][val&(csub-1)];
	  cval>>=csubbits;
	  if(book>=0){
	    if((fit_value[j+k]=vorbis_book_decode(books+book,&vb->opb))==-1)
	      goto eop;
	  }else{
	    fit_value[j+k]=0;
	  }
	}
      }else{
	for(k=0;k<cdim;k++)
	  fit_value[j+k]=0;
      }  
      j+=cdim;
    }

    /* unwrap positive values and reconsitute via linear interpolation */
    for(i=2;i<posts;i++){
      int predicted=render_point(info->postlist[look->loneighbor[i-2]],
				 info->postlist[look->hineighbor[i-2]],
				 fit_value[look->loneighbor[i-2]],
				 fit_value[look->hineighbor[i-2]],
				 info->postlist[i]);
      int hiroom=look->quant_q-predicted;
      int loroom=predicted;
      int room=(hiroom<loroom?hiroom:loroom)<<1;
      int val=fit_value[i];

      if(val>=room){
	if(hiroom>loroom){
	  val = val-loroom;
	}else{
	  val = -1-(val-hiroom);
	}
      }else{
	if(val&1){
	  val= -1 -(val>>1);
	}else{
	  val>>=1;
	}
      }

      fit_value[i]=val+predicted;
    }

    /* render the lines */
    {
      int hx;
      int lx=0;
      int ly=fit_value[0]*info->mult;
      for(j=1;j<posts;j++){
	int current=look->forward_index[j];
	int hy=fit_value[current]*info->mult;
	hx=info->postlist[current];
	
	render_line(lx,hx,ly,hy,out);
	
	lx=hx;
	ly=hy;
      }
      for(j=hx;j<n;j++)out[j]=0.f; /* be certain */
    }    
    return(1);
  }

  /* fall through */
 eop:
  memset(out,0,sizeof(float)*n);
  return(0);
}

/* export hooks */
vorbis_func_floor floor1_exportbundle={
  &floor1_pack,&floor1_unpack,&floor1_look,&floor1_copy_info,&floor1_free_info,
  &floor1_free_look,&floor1_forward,&floor1_inverse
};
