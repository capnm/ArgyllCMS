
/* 
 * Argyll Color Correction System
 * Multi-dimensional regularized splines data fitter
 *
 * Author: Graeme W. Gill
 * Date:   2004/8/14
 *
 * Copyright 1996 - 2009 Graeme W. Gill
 * All rights reserved.
 *
 * This material is licenced under the GNU AFFERO GENERAL PUBLIC LICENSE Version 3 :-
 * see the License.txt file for licencing details.
 */

/*
 * This file contains the scattered data solution specific code.
 *
 * The regular spline implementation was inspired by the following technical reports:
 *
 * D.J. Bone, "Adaptive Multi-Dimensional Interpolation Using Regularized Linear Splines,"
 * Proc. SPIE Vol. 1902, p.243-253, Nonlinear Image Processing IV, Edward R. Dougherty;
 * Jaakko T. Astola; Harold G. Longbotham;(Eds)(1993).
 * 
 * D.J. Bone, "Adaptive Colour Printer Modeling using regularized linear splines,"
 * Proc. SPIE Vol. 1909, p. 104-115, Device-Independent Color Imaging and Imaging
 * Systems Integration, Ricardo J. Motta; Hapet A. Berberian; Eds.(1993)
 *
 * Don Bone and Duncan Stevenson, "Modelling of Colour Hard Copy Devices Using Regularised
 * Linear Splines," Proceedings of the APRS workshop on Colour Imaging and Applications,
 * Canberra (1994)
 *
 * see <http://www.cmis.csiro.au/Don.Bone/>
 *
 * Also of interest was:
 * 
 * "Discrete Smooth Interpolation", Jean-Laurent Mallet, ACM Transactions on Graphics, 
 * Volume 8, Number 2, April 1989, Pages 121-144.
 * 
 */

/* TTBD:
 *
 *	Try simple approach to reduce extrapolation accumulation (edge propogation) effects.
 *	Do this by saving bounding box of scattered points, and then increase smoothness coupling
 *  in direction of axis that is outside this box (or the reverse, reduce smoothness
 *  coupling in direction of any axis that is not outside this box).
 *  [Example is "t3d -t 6 -P 0:0:0:1:1:1" where lins should not bend up at top end.]
 *  
 *  Speedup that skips recomputing all of A to add new points seems OK. (nothing uses
 *  incremental currently anyway.)
 *
 *  Is there any way of speeding up incremental recalculation ????
 *
 * Add optional simplex point interpolation to
 * solve setup. (No large advantage in this ??) 
 *
 * Find a more effective way to mitigate the smoothness "clumping"
 * effect where corners in particular over smooth ?
 *
 * Get rid of error() calls - return status instead
 */

/*
	Scattered data fit related smoothness control.

	We adjust the curve/data point weighting to account for the
	grid resolution (to make it resolution independent), as well
	as allow for the dimensionality (each dimension contributes
	a curvature error).

	The default assumption is that the grid resolution is set
	to matche the input data range for that dimension, eg. if
	a sub range of input space is all that is needed, then a
	smaller grid resolution can/should be used if smoothness
	is expected to remain symetric in relation to the input
	domain.

	eg. Input range 0.0 - 1.0 and 0.0 - 0.5
	   matching res 50        and 25

	The alternative is to set the RSPL_SYMDOMAIN flag,
	in which case the grid resolution is not taken to
	be a measure of the dimension scale, and is assumed
	to be just a lower resolution sampling of the domain.

	eg. Input range 0.0 - 1.0 and 0.0 - 1.0
	   with res.    50        and 25

	still has symetrical smoothness in relation
	to the input domain.


	NOTE :- that both input and output values are normalised
	by the ranges given during rspl construction. The ranges
	set the significance between the input and output values.

	eg. Input ranges 0.0 - 1.0 and 0.0 - 100.0
	(with equal grid resolution)
	will have symetry when measured against the the
	same % change in the input domain, but will
	appear non-symetric if measured against the
	same numerical change.

 */




#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include <time.h>
#if defined(__IBMC__) && defined(_M_IX86)
#include <float.h>
#endif

#include "rspl_imp.h"
#include "numlib.h"
#include "counters.h"	/* Counter macros */

#undef DEBUG			/* Print contents of solution setup etc. */
#undef DEBUG_PROGRESS	/* Print progress of acheiving tollerance target */

#define DEFAVGDEV 0.5	/* Default average deviation % */

/* algorithm parameters [Release defaults] */
#undef POINTWEIGHT		/* [Undef] Increas smoothness weighting proportional to number of points */
#define INCURVEADJ		/* [Defined] Adjust smoothness criteria for input curve grid spacing */
#define EXTRA_SURFACE_SMOOTHING /* [Defined] Stiffen surface points to comp. for single ended. */
						/* The following are available, but the smoothing table is */
						/* not setup for them, and they are not sufficiently different */
						/* from the default smoothing to be useful. */
#define ENABLE_2PASSSMTH	/* [Define] Enable 2 pass smooth using Gaussian filter */
#define ENABLE_EXTRAFIT	/* [Undef] Enable the extra fit option. Good to combat high smoothness. */

#define TWOPASSORDER 2.0	/* Filter order. 2 = Gaussian */

/* Tuning parameters */
#ifdef NEVER

/* Experimental set: */

#pragma message("!!!!!!!!! Experimental config set !!!!!!!!!")

#define TOL 1e-12		/* Tollerance of result - usually 1e-5 is best. */
#define TOL_IMP 1.0		/* Minimum error improvement to continue - reduces accuracy (1.0 == off) */
#undef GRADUATED_TOL	/* Speedup attemp - use reduced tollerance for prior grids. */
#define GRATIO 2.0		/* Multi-grid resolution ratio */
#undef OVERRLX 		/* Use over relaxation factor when progress slows (worse accuracy ?) */
#define JITTERS 0		/* Number of 1D conjugate solve itters */
#define CONJ_TOL 1.0	/* Extra tolereance on 1D conjugate solution times TOL. */
#define MAXNI 16		/* Maximum itteration without checking progress */
//#define SMOOTH 0.000100	/* Set nominal smoothing (1.0) */
#define WEAKW  0.1		/* Weak default function nominal effect (1.0) */
#define ZFCOUNT 1		/* Extra fit repeats */

#else

/* Release set: */

#define TOL 1e-6		/* [1e-6] Tollerance of result - usually 1e-5 is best. */
#define TOL_IMP 0.998	/* [0.998] Minimum error improvement to continue - reduces accuracy (1.0 == off) */
#undef GRADUATED_TOL	/* [Undef] Speedup attemp - use reduced tollerance for prior grids. */
#define GRATIO 2.0		/* [2.0] Multi-grid resolution ratio */
#undef OVERRLX 			/* [Undef] Use over relaxation when progress slows (worse accuracy ?) */
#define JITTERS 0		/* [0] Number of 1D conjugate solve itters */
#define CONJ_TOL 1.0	/* [1.0] Extra tolereance on 1D conjugate solution times TOL. */
#define MAXNI 16		/* [16] Maximum itteration without checking progress */
//#define SMOOTH 0.000100	/* Set nominal smoothing (1.0) */
#define WEAKW  0.1		/* [0.1] Weak default function nominal effect (1.0) */
#define ZFCOUNT 1		/* [1] Extra fit repeats */

#endif

#undef NEVER
#define ALWAYS

/* Implemented in rspl.c: */
extern void alloc_grid(rspl *s);

extern int is_mono(rspl *s);

/* Convention is to use:
   i to index grid points u.a
   n to index data points d.a
   e to index position dimension di
   f to index output function dimension fdi
   j misc and cube corners
   k misc
 */

/* ================================================= */
/* Structure to hold temporary data for multi-grid calculations */
/* One is created for each resolution. Only used in this file. */
struct _mgtmp {
	rspl *s;	/* Associated rspl */
	int f;		/* Output dimension being calculated */

	/* Weak default function stuff */
	double wdfw;			/* Weight per grid point */

	/* Scattered data fit stuff */
	struct {
		double cw[MXDI];	/* Curvature weight factor */
	} sf;

	/* Grid points data */
	struct {
		int res[MXDI];	/* Single dimension grid resolution */
		int bres, brix;	/* Biggest resolution and its index */
		double mres;	/* Geometric mean res[] */
		int no;			/* Total number of points in grid = res ^ di */
		ratai l,h,w;	/* Grid low, high, grid cell width */

		double *ipos[MXDI]; /* Optional relative grid cell position for each input dim cell */

		/* Grid array offset lookups */
		int ci[MXRI];		/* Grid coordinate increments for each dimension */
		int hi[POW2MXRI];	/* Combination offset for sequence through cube. */
	} g;

	/* Data point grid dependent information */
	struct mgdat {
		int b;				/* Index for associated base grid point, in grid points */
		double w[POW2MXRI];	/* Weight for surrounding gridpoints [2^di] */
	} *d;

	/* Equation Solution related (Grid point solutions) */
	struct {
		double **ccv;		/* [gno][di] Curvature Compensation Values */
		double **A;			/* A matrix of interpoint weights A[g.no][q.acols] */
		int acols;			/* A matrix columns needed */
							/* Packed indexes run from 0..acols-1 */
							/* Sparse index allows for +/-2 offset in any one dimension */
							/* and +/-1 offset in all dimensions, but only the +ve offset */
							/* half of the sparse matrix is stored, due to equations */
							/* being symetrical. */
		int xcol[HACOMPS+8];/* A array column translation from packed to sparse index */ 
		int *ixcol;			/* A array column translation from sparse to packed index */ 
		double *b;			/* b vector for RHS of simultabeous equation b[g.no] */
		double normb;		/* normal of b vector */
		double *x;			/* x solution to A . x = b */
	} q;

}; typedef struct _mgtmp mgtmp;


/* ================================================= */
/* Temporary arrays used by cj_line(). We try and avoid */
/* allocating and de-allocating these, and merely expand */
/* them as needed */
typedef struct {
	double *z, *xx, *q, *r;
	double *n;
	int l_nid;
} cj_arrays;
static void init_cj_arrays(cj_arrays *ta);
static void free_cj_arrays(cj_arrays *ta);

static int add_rspl_imp(rspl *s, int flags, void *d, int dtp, int dno);
static mgtmp *new_mgtmp(rspl *s, int gres[MXDI], int f);
static void free_mgtmp(mgtmp *m);
static void setup_solve(mgtmp *m, int final);
static void solve_gres(mgtmp *m, cj_arrays *ta, double tol, int final);
static void init_soln(mgtmp  *m1, mgtmp  *m2);
static void comp_ccv(mgtmp *m);
static void filter_ccv(rspl *s, double stdev);
static void init_ccv(mgtmp *m);
static void comp_extrafit_corr(mgtmp *m);

/* Initialise the regular spline from scattered data */
/* Return non-zero if non-monotonic */
static int
fit_rspl_imp(
	rspl *s,		/* this */
	int flags,		/* Combination of flags */
	void *d,		/* Array holding position and function values of data points */
	int dtp,		/* Flag indicating data type, 0 = (co *), 1 = (cow *), 2 = (coww *) */
	int dno,		/* Number of data points */
	ratai glow,		/* Grid low scale - will be expanded to enclose data, NULL = default 0.0 */
	ratai ghigh,	/* Grid high scale - will be expanded to enclose data, NULL = default 1.0 */
	int gres[MXDI],	/* Spline grid resolution */
	ratao vlow,		/* Data value low normalize, NULL = default 0.0 */
	ratao vhigh,	/* Data value high normalize - NULL = default 1.0 */
	double smooth,	/* Smoothing factor, 0.0 = default 1.0 */
					/* (if -ve, overides optimised smoothing, and sets raw smoothing */
					/*  typically between 1e-7 .. 1e-1) */
	double avgdev[MXDO],
	                /* Average Deviation of function values as proportion of function range, */
					/* typical value 0.005 (aprox. = 0.564 times the standard deviation) */
					/* NULL = default 0.005 */
	double *ipos[MXDI], /* Optional relative grid cell position for each input dim cell, */
					/* gres[] entries per dimension. Used to scale smoothness criteria */
	double weak,	/* Weak weighting, nominal = 1.0 */
	void *dfctx,	/* Opaque weak default function context */
	void (*dfunc)(void *cbntx, double *out, double *in)		/* Function to set from, NULL if none. */
) {
	int di = s->di, fdi = s->fdi;
	int i, e, f;

#ifdef NEVER
printf("~1 rspl: gres = %d %d %d %d, smooth = %f, avgdev = %f %f %f\n",
gres[0], gres[1], gres[2], gres[3], smooth, avgdev[0], avgdev[1], avgdev[2]);
printf("~1 rspl: glow = %f %f %f %f ghigh = %f %f %f %f\n",
glow[0], glow[1], glow[2], glow[3], ghigh[0], ghigh[1], ghigh[2], ghigh[3]);
printf("~1 rspl: vlow = %f %f %f vhigh = %f %f %f\n",
vlow[0], vlow[1], vlow[2], vhigh[0], vhigh[1], vhigh[2]);
printf("~1 rspl: flags = 0x%x\n",flags);
#endif

#if defined(__IBMC__) && defined(_M_IX86)
	_control87(EM_UNDERFLOW, EM_UNDERFLOW);
#endif

	/* This is a restricted size function */
	if (di > MXRI)
		error("rspl: fit can't handle di = %d",di);
	if (fdi > MXRO)
		error("rspl: fit can't handle fdi = %d",fdi);

	/* set debug level */
	s->debug = (flags >> 24);

	/* Init other flags */
	if (flags & RSPL_VERBOSE)	/* Turn on progress messages to stdout */
		s->verbose = 1;
	if (flags & RSPL_NOVERBOSE)	/* Turn off progress messages to stdout */
		s->verbose = 0;

#ifdef ENABLE_2PASSSMTH
	s->tpsm = (flags & RSPL_2PASSSMTH) ? 1 : 0;		/* Enable 2 pass smoothing */
#endif
#ifdef ENABLE_EXTRAFIT
	s->zf = (flags & RSPL_EXTRAFIT2) ? 2 : 0;		/* Enable extra fitting effort */
#endif
	s->symdom = (flags & RSPL_SYMDOMAIN) ? 1 : 0;	/* Turn on symetric smoothness with gres */

	/* Save smoothing factor and Average Deviation */
	s->smooth = smooth;
	if (avgdev != NULL) {
		for (f = 0; f < s->fdi; f++)
			s->avgdev[f] = avgdev[f];
	} else {
		for (f = 0; f < s->fdi; f++)
			s->avgdev[f] = DEFAVGDEV/100.0;
	}

	/* Save weak default function information */
	s->weak = weak;
	s->dfctx = dfctx;
	s->dfunc = dfunc;

	/* Init data point storage to zero */
	s->d.no = 0;
	s->d.a = NULL;

	/* record low and high grid range */
	s->g.mres = 1.0;
	s->g.bres = 0;
	for (e = 0; e < s->di; e++) {
		if (gres[e] < 2)
			error("rspl: grid res must be >= 2!");
		s->g.res[e] = gres[e]; /* record the desired resolution of the grid */
		s->g.mres *= gres[e];
		if (gres[e] > s->g.bres) {
			s->g.bres = gres[e];
			s->g.brix = e;
		}

		if (glow == NULL)
			s->g.l[e] = 0.0;
		else
			s->g.l[e] = glow[e];

		if (ghigh == NULL)
			s->g.h[e] = 1.0;
		else
			s->g.h[e] = ghigh[e];
	}
	s->g.mres = pow(s->g.mres, 1.0/e);		/* geometric mean */

	/* record low and high data normalizing factors */
	for (f = 0; f < s->fdi; f++) {
		if (vlow == NULL)
			s->d.vl[f] = 0.0;
		else
			s->d.vl[f] = vlow[f];

		if (vhigh == NULL)
			s->d.vw[f] = 1.0;
		else
			s->d.vw[f] = vhigh[f];
	}

	/* If we are supplied initial data points, expand the */
	/* grid range to be able to cover it. */
	/* Also compute average data value. */
	for (f = 0; f < s->fdi; f++)
		s->d.va[f] = 0.5;	/* default average */
	if (dtp == 0) {			/* Default weight */
		co *dp = (co *)d;

		for (i = 0; i < dno; i++) {
			for (e = 0; e < s->di; e++) {
				if (dp[i].p[e] > s->g.h[e])
					s->g.h[e] = dp[i].p[e];
				if (dp[i].p[e] < s->g.l[e])
					s->g.l[e] = dp[i].p[e];
			}
			for (f = 0; f < s->fdi; f++) {
				if (dp[i].v[f] > s->d.vw[f])
					s->d.vw[f] = dp[i].v[f];
				if (dp[i].v[f] < s->d.vl[f])
					s->d.vl[f] = dp[i].v[f];
				s->d.va[f] += dp[i].v[f];
			}
		}
	} else if (dtp == 1) {		/* Per data point weight */
		cow *dp = (cow *)d;

		for (i = 0; i < dno; i++) {
			for (e = 0; e < s->di; e++) {
				if (dp[i].p[e] > s->g.h[e])
					s->g.h[e] = dp[i].p[e];
				if (dp[i].p[e] < s->g.l[e])
					s->g.l[e] = dp[i].p[e];
			}
			for (f = 0; f < s->fdi; f++) {
				if (dp[i].v[f] > s->d.vw[f])
					s->d.vw[f] = dp[i].v[f];
				if (dp[i].v[f] < s->d.vl[f])
					s->d.vl[f] = dp[i].v[f];
				s->d.va[f] += dp[i].v[f];
			}
		}
	} else {				/* Per data point output weight */
		coww *dp = (coww *)d;

		for (i = 0; i < dno; i++) {
			for (e = 0; e < s->di; e++) {
				if (dp[i].p[e] > s->g.h[e])
					s->g.h[e] = dp[i].p[e];
				if (dp[i].p[e] < s->g.l[e])
					s->g.l[e] = dp[i].p[e];
			}
			for (f = 0; f < s->fdi; f++) {
				if (dp[i].v[f] > s->d.vw[f])
					s->d.vw[f] = dp[i].v[f];
				if (dp[i].v[f] < s->d.vl[f])
					s->d.vl[f] = dp[i].v[f];
				s->d.va[f] += dp[i].v[f];
			}
		}
	}
	if (dno > 0) {		/* Complete the average */
		for (f = 0; f < s->fdi; f++)
			s->d.va[f] = (s->d.va[f] - 0.5)/((double)dno);
	}

	/* compute (even division) width of each grid cell */
	for (e = 0; e < s->di; e++) {
		s->g.w[e] = (s->g.h[e] - s->g.l[e])/(double)(s->g.res[e]-1);
	}

	/* Convert low and high to low and width data range */
	for (f = 0; f < s->fdi; f++) {
		s->d.vw[f] -= s->d.vl[f];
	}

#ifdef INCURVEADJ
	/* Save grid cell (smooth data space) position information (if any), */
	if (ipos != NULL) {
		for (e = 0; e < s->di; e++) {
			if (ipos[e] != NULL) {
				if ((s->g.ipos[e] = (double *)calloc(s->g.res[e], sizeof(double))) == NULL)
					error("rspl: malloc failed - ipos[]");
				for (i = 0; i < s->g.res[e]; i++) {
					s->g.ipos[e][i] = ipos[e][i];
					if (i > 0 && fabs(s->g.ipos[e][i] - s->g.ipos[e][i-1]) < 1e-12)
						error("rspl: ipos[%d][%d] to ipos[%d][%d] is nearly zero!",e,i,e,i-1);
				}
			}
		}
	}
#endif /* INCURVEADJ */

	/* Allocate the grid data */
	alloc_grid(s);
	
	/* Zero out the re-usable mgtmps */
	for (f = 0; f < s->fdi; f++) {
		s->mgtmps[f] = NULL;
	}

	{
		int sres;		/* Starting resolution */
		double res;
		double gratio;

		/* Figure out how many multigrid steps to use */
		sres = 4;		/* Else start at minimum grid res of 4 */

		/* Calculate the resolution scaling ratio and number of itters. */
		gratio = GRATIO;
		if (((double)s->g.bres/(double)sres) <= gratio) {
			s->niters = 2;
			gratio = (double)s->g.bres/(double)sres;
		} else {	/* More than one needed */
			s->niters = (int)((log((double)s->g.bres) - log((double)sres))/log(gratio) + 0.5);
			gratio = exp((log((double)s->g.bres) - log((double)sres))/(double)s->niters);
			s->niters++;
		}
		
		/* Allocate space for resolutions and mgtmps pointers */
		if ((s->ires = imatrix(0, s->niters, 0, s->di)) == NULL)
			error("rspl: malloc failed - ires[][]");

		for (f = 0; f < s->fdi; f++) {
			if ((s->mgtmps[f] = (void *) calloc(s->niters, sizeof(void *))) == NULL)
				error("rspl: malloc failed - mgtmps[]");
		}

		/* Fill in the resolution values for each itteration */
		for (res = (double)sres, i = 0; i < s->niters; i++) {
			int ires;

			ires = (int)(res + 0.5);
			for (e = 0; e < s->di; e++) {
				if ((ires + 1) >= s->g.res[e])	/* If close enough biger than target res. */
					s->ires[i][e] = s->g.res[e];
				else
					s->ires[i][e] = ires;
			}
			res *= gratio;
		}

		/* Assert */
		for (e = 0; e < s->di; e++) {
			if (s->ires[s->niters-1][e] != s->g.res[e])
				error("rspl: internal error, final res %d != intended res %d\n",
				       s->ires[s->niters-1][e], s->g.res[e]);
		}

	}

	/* Do the data point fitting */
	return add_rspl_imp(s, 0, d, dtp, dno);
}

double adjw[21] = {
	7.0896971822529019e-278, 2.7480236142217909e+233, 1.4857837676559724e+166,
	1.3997102851752585e-152, 1.3987140593588909e-076, 2.8215833239257504e+243,
	1.4104974786556771e+277, 2.0916973891832284e+121, 2.0820139887245793e-152,
	1.0372833042501621e-152, 2.1511212233835046e-313, 7.7791723264397072e-260,
	6.7035744954188943e+223, 8.5733372291341995e+170, 1.4275976773846279e-071,
	2.3994297542685112e-038, 3.9052141785471924e-153, 3.8223903939904297e-096,
	3.2368131456774088e+262, 6.5639459298208554e+045, 2.0087765219520138e-139
};

/* Do the work of initialising from initial data points. */
/* Return non-zero if non-monotonic */
static int
add_rspl_imp(
	rspl *s,		/* this */
	int flags,		/* Combination of flags */
	void *d,		/* Array holding position and function values of data points */
	int dtp,		/* Flag indicating data type, 0 = (co *), 1 = (cow *), 2 = (coww *) */
	int dno			/* Number of data points */
) {
	int fdi = s->fdi;
	int i, n, e, f;
	cj_arrays ta;	/* cj_line temporary arrays */

	if (flags & RSPL_VERBOSE)	/* Turn on progress messages to stdout */
		s->verbose = 1;
	if (flags & RSPL_NOVERBOSE)	/* Turn off progress messages to stdout */
		s->verbose = 0;

	if (dno == 0) {	/* There are no points to initialise from */
		return 0;
	}

	/* Allocate space for points */
	/* Allocate the scattered data space */
	if ((s->d.a = (rpnts *) malloc(sizeof(rpnts) * dno)) == NULL)
		error("rspl malloc failed - data points");

	/* Add the points */
	if (dtp == 0) {			/* Default weight */
		co *dp = (co *)d;

		/* Append the list into data points */
		for (i = 0, n = s->d.no; i < dno; i++, n++) {
			for (e = 0; e < s->di; e++)
				s->d.a[n].p[e] = dp[i].p[e];
			for (f = 0; f < s->fdi; f++) {
				s->d.a[n].cv[f] =
				s->d.a[n].v[f] = dp[i].v[f];
				s->d.a[n].k[f] = 1.0;		/* Assume all data points have same weight */
			}
		}
	} else if (dtp == 1) {			/* Per data point weight */
		cow *dp = (cow *)d;

		/* Append the list into data points */
		for (i = 0, n = s->d.no; i < dno; i++, n++) {
			for (e = 0; e < s->di; e++)
				s->d.a[n].p[e] = dp[i].p[e];
			for (f = 0; f < s->fdi; f++) {
				s->d.a[n].cv[f] =
				s->d.a[n].v[f] = dp[i].v[f];
				s->d.a[n].k[f] = dp[n].w;	/* Weight specified */
			}
		}
	} else {				/* Per data point output weight */
		coww *dp = (coww *)d;

		/* Append the list into data points */
		for (i = 0, n = s->d.no; i < dno; i++, n++) {
			for (e = 0; e < s->di; e++)
				s->d.a[n].p[e] = dp[i].p[e];
			for (f = 0; f < s->fdi; f++) {
				s->d.a[n].cv[f] =
				s->d.a[n].v[f] = dp[i].v[f];
				s->d.a[n].k[f] = dp[n].w[f];	/* Weight specified */
			}
		}
	}
	s->d.no = dno;

	init_cj_arrays(&ta);		/* Zero temporary arrays */
	
	if (s->verbose && s->zf)
		printf("Doing extra fitting\n");

	/* Do fit of grid to data for each output dimension */
	for (f = 0; f < fdi; f++) {
		int nn = 0;				/* Multigreid resolution itteration index */
		int zfcount = ZFCOUNT;	/* Number of extra fit adjustments to do */
		int donezf = 0;			/* Count - number of extra fit adjustments done */
		float *gp;
		mgtmp *m = NULL;

		for (donezf = 0; donezf <= s->zf; donezf++) {	/* For each extra fit pass */

			for (s->tpsm2 = 0; s->tpsm2 <= s->tpsm; s->tpsm2++) {	/* For passes of 2 pass smoothing */
 
				/* For each resolution (itteration) */
				for (nn = 0; nn < s->niters; nn++) {

					m = new_mgtmp(s, s->ires[nn], f);
					s->mgtmps[f][nn] = (void *)m;

					if (s->tpsm && s->tpsm2 != 0) {	/* 2nd pass of 2 pass smoothing */
						init_ccv(m);				/* Downsample m->ccv from s->g.ccv */
					}
//					setup_solve(m, nn == (s->niters-1));
					setup_solve(m, 1);

					if (nn == 0) {						/* Make sure we have an initial x[] */
						for (i = 0; i <  m->g.no; i++)
							m->q.x[i] = s->d.va[f];		/* Start with average data value */
					} else {
						init_soln(m, s->mgtmps[f][nn-1]);	/* Scale from previous resolution */

						free_mgtmp(s->mgtmps[f][nn-1]);	/* Free previous grid res solution */
						s->mgtmps[f][nn-1] = NULL;
					}

					solve_gres(m, &ta,
#if defined(GRADUATED_TOL)
					              TOL * s->g.res[s->g.brix]/s->ires[nn][s->g.brix],
#else
					              TOL,
#endif
					              s->ires[nn][s->g.brix] >= s->g.res[s->g.brix]);	/* Use itterative */

				}	/* Next resolution */

				if (s->tpsm && s->tpsm2 == 0) {
					double fstdev;		/* Filter standard deviation */
//printf("~1 setting up second pass smoothing !!!\n");

					/* Compute the curvature compensation values from */
					/* first pass final resolution */
					comp_ccv(m);

					if (s->smooth >= 0.0) {
						/* Compute from: no dim, no data points, avgdev & extrafit */
						fstdev = 0.05 * s->smooth;
fprintf(stderr,"~1 !!! Gaussian smoothing not being computed Using default %f !!!\n",fstdev);
					} else {	/* Special used to calibrate table */
						fstdev = -s->smooth;
					}
//fprintf(stderr,"~1 Gaussian smoothing with fstdev %f !!!\n",fstdev);
					/* Smooth the ccv's */
					filter_ccv(s, fstdev);
				}
			}	/* Next two pass smoothing pass */
			if (s->zf)
				comp_extrafit_corr(m);		/* Compute correction to data target values */
		}	/* Next extra fit pass */

		/* Clean up after 2 pass smoothing */
		s->tpsm2 = 0;
		if (s->g.ccv != NULL) {
			free_dmatrix(s->g.ccv, 0, s->g.no-1, 0, s->di-1);
			s->g.ccv = NULL;
		}

		/* Transfer result in x[] to appropriate grid point value */
		for (gp = s->g.a, i = 0; i < s->g.no; gp += s->g.pss, i++)
			gp[f] = (float)m->q.x[i];

		free_mgtmp(s->mgtmps[f][nn-1]);		/* Free final resolution entry */
		s->mgtmps[f][nn-1] = NULL;

	}	/* Next output channel */

	/* Free up cj_line temporary arrays */
	free_cj_arrays(&ta);

	/* Return non-mono check */
	return is_mono(s);
}

/* Initialise the regular spline from scattered data */
/* Return non-zero if non-monotonic */
static int
fit_rspl(
	rspl *s,		/* this */
	int flags,		/* Combination of flags */
	co *d,			/* Array holding position and function values of data points */
	int dno,		/* Number of data points */
	ratai glow,		/* Grid low scale - will be expanded to enclose data, NULL = default 0.0 */
	ratai ghigh,	/* Grid high scale - will be expanded to enclose data, NULL = default 1.0 */
	int gres[MXDI],	/* Spline grid resolution */
	ratao vlow,		/* Data value low normalize, NULL = default 0.0 */
	ratao vhigh,	/* Data value high normalize - NULL = default 1.0 */
	double smooth,	/* Smoothing factor, nominal = 1.0 */
	double avgdev[MXDO],
	                /* Average Deviation of function values as proportion of function range. */
	double *ipos[MXDI] /* Optional relative grid cell position for each input dim cell, */
					/* gres[] entries per dimension. Used to scale smoothness criteria */
) {
	/* Call implementation with (co *) data */
	return fit_rspl_imp(s, flags, (void *)d, 0, dno, glow, ghigh, gres, vlow, vhigh,
	                    smooth, avgdev, ipos, 1.0, NULL, NULL);
}

/* Initialise the regular spline from scattered data with weights */
/* Return non-zero if non-monotonic */
static int
fit_rspl_w(
	rspl *s,		/* this */
	int flags,		/* Combination of flags */
	cow *d,			/* Array holding position, function and weight values of data points */
	int dno,		/* Number of data points */
	ratai glow,		/* Grid low scale - will be expanded to enclose data, NULL = default 0.0 */
	ratai ghigh,	/* Grid high scale - will be expanded to enclose data, NULL = default 1.0 */
	int gres[MXDI],		/* Spline grid resolution */
	ratao vlow,		/* Data value low normalize, NULL = default 0.0 */
	ratao vhigh,	/* Data value high normalize - NULL = default 1.0 */
	double smooth,	/* Smoothing factor, nominal = 1.0 */
	double avgdev[MXDO],
	                /* Average Deviation of function values as proportion of function range. */
	double *ipos[MXDI] /* Optional relative grid cell position for each input dim cell, */
					/* gres[] entries per dimension. Used to scale smoothness criteria */
) {
	/* Call implementation with (cow *) data */
	return fit_rspl_imp(s, flags, (void *)d, 1, dno, glow, ghigh, gres, vlow, vhigh,
	                    smooth, avgdev, ipos, 1.0, NULL, NULL);
}

/* Initialise the regular spline from scattered data with individual weights */
/* Return non-zero if non-monotonic */
static int
fit_rspl_ww(
	rspl *s,		/* this */
	int flags,		/* Combination of flags */
	coww *d,		/* Array holding position, function and weight values of data points */
	int dno,		/* Number of data points */
	ratai glow,		/* Grid low scale - will be expanded to enclose data, NULL = default 0.0 */
	ratai ghigh,	/* Grid high scale - will be expanded to enclose data, NULL = default 1.0 */
	int gres[MXDI],		/* Spline grid resolution */
	ratao vlow,		/* Data value low normalize, NULL = default 0.0 */
	ratao vhigh,	/* Data value high normalize - NULL = default 1.0 */
	double smooth,	/* Smoothing factor, nominal = 1.0 */
	double avgdev[MXDO],
	                /* Average Deviation of function values as proportion of function range. */
	double *ipos[MXDI] /* Optional relative grid cell position for each input dim cell, */
					/* gres[] entries per dimension. Used to scale smoothness criteria */
) {
	/* Call implementation with (cow *) data */
	return fit_rspl_imp(s, flags, (void *)d, 2, dno, glow, ghigh, gres, vlow, vhigh,
	                    smooth, avgdev, ipos, 1.0, NULL, NULL);
}

/* Initialise from scattered data, with weak default function. */
/* Return non-zero if result is non-monotonic */
static int
fit_rspl_df(
	rspl *s,		/* this */
	int flags,		/* Combination of flags */
	co *d,			/* Array holding position and function values of data points */
	int dno,		/* Number of data points */
	datai glow,		/* Grid low scale - will expand to enclose data, NULL = default 0.0 */
	datai ghigh,	/* Grid high scale - will expand to enclose data, NULL = default 1.0 */
	int gres[MXDI],	/* Spline grid resolution, ncells = gres-1 */
	datao vlow,		/* Data value low normalize, NULL = default 0.0 */
	datao vhigh,	/* Data value high normalize - NULL = default 1.0 */
	double smooth,	/* Smoothing factor, nominal = 1.0 */
	double avgdev[MXDO],
	                /* Average Deviation of function values as proportion of function range. */
	double *ipos[MXDI], /* Optional relative grid cell position for each input dim cell, */
					/* gres[] entries per dimension. Used to scale smoothness criteria */
	double weak,	/* Weak weighting, nominal = 1.0 */
	void *cbntx,	/* Opaque function context */
	void (*func)(void *cbntx, double *out, double *in)		/* Function to set from */
) {
	/* Call implementation with (co *) data */
	return fit_rspl_imp(s, flags, (void *)d, 0, dno, glow, ghigh, gres, vlow, vhigh,
	                    smooth, avgdev, ipos, weak, cbntx, func);
}

/* Initialise from scattered data, with per point weighting and weak default function. */
/* Return non-zero if result is non-monotonic */
static int
fit_rspl_w_df(
	rspl *s,	/* this */
	int flags,		/* Combination of flags */
	cow *d,			/* Array holding position, function and weight values of data points */
	int dno,		/* Number of data points */
	datai glow,		/* Grid low scale - will expand to enclose data, NULL = default 0.0 */
	datai ghigh,	/* Grid high scale - will expand to enclose data, NULL = default 1.0 */
	int gres[MXDI],	/* Spline grid resolution, ncells = gres-1 */
	datao vlow,		/* Data value low normalize, NULL = default 0.0 */
	datao vhigh,	/* Data value high normalize - NULL = default 1.0 */
	double smooth,	/* Smoothing factor, nominal = 1.0 */
	double avgdev[MXDO],
	                /* Average Deviation of function values as proportion of function range. */
	double *ipos[MXDI], /* Optional relative grid cell position for each input dim cell, */
					/* gres[] entries per dimension. Used to scale smoothness criteria */
	double weak,	/* Weak weighting, nominal = 1.0 */
	void *cbntx,	/* Opaque function context */
	void (*func)(void *cbntx, double *out, double *in)		/* Function to set from */
) {
	/* Call implementation with (cow *) data */
	return fit_rspl_imp(s, flags, (void *)d, 1, dno, glow, ghigh, gres, vlow, vhigh,
	                    smooth, avgdev, ipos, weak, cbntx, func);
}

/* Init scattered data elements in rspl */
void
init_data(rspl *s) {
	s->d.no = 0;
	s->d.a = NULL;
	s->fit_rspl      = fit_rspl;
	s->fit_rspl_w    = fit_rspl_w;
	s->fit_rspl_ww   = fit_rspl_ww;
	s->fit_rspl_df   = fit_rspl_df;
	s->fit_rspl_w_df = fit_rspl_w_df;
}

/* Free the scattered data allocation */
void
free_data(rspl *s) {
	int i, f;

	if (s->ires != NULL) {
		free_imatrix(s->ires, 0, s->niters, 0, s->di);
		s->ires = NULL;
	}

	/* Free up mgtmps */
	for (f = 0; f < s->fdi; f++) {
		if (s->mgtmps[f] != NULL) {
			for (i = 0; i < s->niters; i++) {
				if (s->mgtmps[f][i] != NULL) {
					free_mgtmp(s->mgtmps[f][i]);
				}
			}
			free(s->mgtmps[f]);
			s->mgtmps[f] = NULL;
		}
	}

	if (s->d.a != NULL) {	/* Free up the data point data */
		free((void *)s->d.a);
		s->d.a = NULL;
	}
}

/* - - - - - - - - - - - - - - - - - - - - - - - -*/
/* In theory, the smoothness should increase proportional to the square of the */
/* overall average sample deviation. (Or the weight of each individual data point */
/* could be made inversely proportional to the square of its average sample */
/* deviation, or square of its standard deviation, or its variance, etc.) */
/* In practice, other factors also seem to come into play, so we use a */
/* table to lookup an "optimal" smoothing factor for each combination */
/* of the parameters dimension, sample count and average sample deviation. */

/* The contents of the table were created by taking some representative */
/* profiles and testing them with various numbers of data points */
/* and added L*a*b* noise, and locating the optimal smoothing factor */
/* for each parameter. */ 
/* If the instrument variance is assumed to be a constant factor */
/* in the sensors, then it would be appropriate to modify the */
/* data weighting rather than the overall smoothness, */
/* since a constant XYZ variance could be transformed into a */
/* per data point L*a*b* variance. */
/* The optimal smoothness factor doesn't appear to have any significant */
/* dependence on the RSPL resolution. */

/* Return an appropriate smoothing factor for the combination of final parameters. */
/* This is a base value that will be multiplied by the extra supplied smoothing factor. */
/* The "Average sample deviation" is a measure of its randomness. */
/* For instance, values that had a +/- 0.1 uniform random error added */
/* to them, would have an average sample deviation of 0.05. */
/* For normally distributed errors, the average deviation is */
/* aproximately 0.564 times the standard deviation. (0.564 * sqrt(variance)) */
/* This table is appropriate for the default rspl algorithm + slight EXTRA_SURFACE_SMOOTHING, */
/* and is NOT setup for RSPL_2PASSSMTH or RSPL_EXTRAFIT2 !! */
/* SMOOTH */
// ~~99
static double opt_smooth(
	rspl *s,
	int di,		/* Dimensions */
	int ndp,	/* Number of data points */
	double ad	/* Average sample deviation (proportion of input range) */
) {
	int i;
	double nc;		/* Normalised sample count */
	double lsm, sm, tweakf;

	/* Lookup that converts the di'th root of the data point count */
	/* into the smf table row index */
	int ncixN;
	int ncix;		/* Normalised sample count index */
	double ncw;		/* Weight of [ncix], 1-weight of [ncix+1] */ 
	int nncixv[4] = { 6, 6, 10, 11 };		/* Number in ncixv[] rows */
	double ncixv[4][11] = {				/* nc to smf index */
	   { 5.0, 10.0, 20.0, 50.0, 100.0, 200.0 },
	   { 5.0, 10.0, 20.0, 50.0, 100.0, 200.0 },
	   { 2.92, 3.68, 4.22,  5.0, 6.3, 7.94, 10.0, 12.6,  20.0, 50.0 },
	   { 2.66, 3.16, 3.76, 4.61, 5.0, 5.48,  6.51, 7.75, 10.0, 20.0, 31.62 }
	};

	/* Lookup that converts the deviation fraction */
	/* into the smf table column index */
	int adixN;		/* Number in array */
	int adix;		/* Average deviation count index */
	double adw;		/* Weight of [adix], 1-weight of [adix+1] */ 
	int nadixv[4] = { 6, 6, 6, 7 };		/* Number in adixv[] rows */
	double adixv[4][7] = { /* ad to smf index */
		{ 0.0001, 0.0025, 0.005, 0.0125, 0.025, 0.05 },
		{ 0.0001, 0.0025, 0.005, 0.0125, 0.025, 0.05 },
		{ 0.0001, 0.0025, 0.005, 0.0125, 0.025, 0.05 },
		{ 0.0001, 0.0025, 0.005, 0.0075, 0.0125, 0.025, 0.05 }
	};
	

	/* New for V1.10, from smtmpp using sRGB, EpsonR1800, Hitachi2112, */
	/* Fogra39L, Canon1180, Epson10K, with low EXTRA_SURFACE_SMOOTHING. */

	/* Main lookup table, by [di][ncix][adix]: */
	/* Values are log of smoothness value. */
	static double smf[4][11][7] = {
		/* 1D: */
		{
/* -r value:   0     0.25% 0.5%  1.25% 2.5%  5%	 */
/* Tot white N 0%    1%    2%    5%    10%   20% */
/* 5 */		{ -5.0, -5.3, -5.2, -4.4, -3.5, -0.8 },
/* 10 */	{ -6.4, -5.6, -5.1, -4.5, -4.0, -3.6 },
/* 20 */	{ -6.4, -5.9, -5.5, -4.6, -3.9, -3.3 },
/* 50 */	{ -6.8, -6.0, -5.6, -4.9, -4.4, -3.7 },
/* 100 */	{ -6.9, -6.2, -5.6, -4.9, -4.3, -3.5 },
/* 200 */	{ -6.9, -5.9, -5.5, -5.1, -4.7, -4.4 }
		},
		/* 2D: */
		{
			/* 0%    1%    2%    5%    10%   20% */
/* 5 */		{ -5.0, -5.0, -5.0, -4.8, -4.2, -3.2 },
/* 10 */	{ -5.1, -4.9, -4.6, -3.9, -3.3, -2.6 },
/* 20 */	{ -5.9, -5.0, -4.6, -4.1, -3.6, -3.1 },
/* 50 */	{ -6.7, -5.1, -4.7, -4.2, -3.7, -3.1 },
/* 100 */	{ -6.8, -5.0, -4.6, -4.0, -3.6, -3.0 },
/* 200 */	{ -6.8, -4.9, -4.4, -3.9, -3.5, -3.1 }
		},
		/* 3D: */
		{
			/* 0%    1%    2%    5%    10%   20% */
/* 2.92 */	{ -5.2, -5.0, -5.0, -4.9, -3.6, -2.2 },
/* 3.68 */	{ -5.5, -5.6, -5.6, -5.2, -4.4, -2.4 },
/* 4.22 */	{ -4.7, -4.8, -5.7, -5.9, -5.9, -2.3 },
/* 5.00 */	{ -4.1, -4.1, -5.0, -3.8, -3.4, -2.6 },
/* 6.30 */	{ -4.8, -4.6, -4.6, -4.1, -3.8, -3.4 },
/* 7.94 */	{ -4.7, -4.7, -4.7, -3.8, -3.3, -2.9 },
/* 10.0 */	{ -4.7, -4.8, -4.6, -3.9, -3.4, -3.0 },
/* 12.6 */	{ -5.2, -4.7, -4.4, -4.0, -3.4, -2.9 },
/* 20.0 */	{ -5.5, -5.0, -4.3, -3.6, -3.1, -2.8 },
/* 50.0 */	{ -5.1, -4.7, -4.3, -3.8, -3.3, -2.8 }

		},
		/* 4D: */
		{
			/* 0%    1%    2%    3%,   5%    10%   20% */
/* 2.66 */	{ -5.5, -5.6, -4.9, -4.8, -4.5, -2.8, -3.1 },
/* 3.16 */	{ -4.3, -4.2, -4.0, -3.6, -3.2, -2.8, -2.6 },
/* 3.76 */	{ -4.3, -4.2, -4.0, -3.8, -3.2, -2.8, -1.5 },
/* 4.61 */	{ -4.5, -3.9, -3.5, -3.2, -3.0, -2.4, -1.9 },
/* 5.00 */	{ -4.5, -4.3, -3.7, -3.3, -3.0, -2.3, -1.9 },
/* 5.48 */	{ -4.7, -4.5, -4.3, -3.9, -3.2, -2.0, -0.9 },
/* 6.51 */	{ -4.3, -4.3, -4.1, -3.9, -3.1, -2.3, -1.6 },
/* 7.75 */	{ -4.5, -4.4, -3.8, -3.5, -3.1, -2.4, -1.6 },
/* 10.00 */	{ -4.9, -4.3, -3.6, -3.2, -2.8, -2.2, -1.6 },
/* 20.00 */	{ -4.8, -3.5, -3.0, -2.8, -2.5, -2.2, -1.9 },
/* 31.62 */	{ -5.1, -3.7, -3.0, -2.7, -2.3, -1.9, -1.5 }
		}
	};

	/* Smoothness tweak */
	static double tweak[21] = {
		8.0891733310676571e-263, 1.1269230397087924e+243, 5.5667427967136639e+170,
		4.6422059659371074e-072, 4.7573037006103243e-038, 2.2050803446598081e-152,
		1.9082109674254010e-094, 1.2362202651281476e+262, 1.8334727652805863e+044,
		1.7193993129127580e-139, 8.4028172720870109e-316, 7.7791723264393403e-260,
		4.5505694361996285e+198, 1.4450789782663302e+214, 4.8548304485951407e-033,
		6.0848773033767158e-153, 2.2014810203887549e+049, 6.0451581453053059e-153,
		4.5657997262605343e+233, 1.1415770815909824e+243, 2.0087364177250134e-139
	};

	/* Real world correction factors go here - */
	/* None needed at the moment ? */
	double rwf[4] = { 1.0, 1.0, 1.0, 1.0 };		/* Factor for each dimension */

//printf("~1 opt_smooth called with di = %d, ndp = %d, ad = %f\n",di,ndp,ad);
	if (di < 1)
		di = 1;
	nc = pow((double)ndp, 1.0/(double)di);		/* Normalised sample count */
	if (di > 4)
		di = 4;
	di--;			/* Make di 0..3 */

	/* Convert the two input parameters into appropriate */
	/* indexes and weights for interpolation. We assume ratiometric scaling. */

	/* Number of samples */
	ncixN = nncixv[di];
	if (nc <= ncixv[di][0]) {
		ncix = 0;
		ncw = 1.0;
	} else if (nc >= ncixv[di][ncixN-1]) {
		ncix = ncixN-2;
		ncw = 0.0;
	} else {
		for (ncix = 0; ncix < ncixN; ncix++) {
			if (nc >= ncixv[di][ncix] && nc <= ncixv[di][ncix+1])
				break;
			
		}
		ncw = 1.0 - (log(nc) - log(ncixv[di][ncix]))
		           /(log(ncixv[di][ncix+1]) - log(ncixv[di][ncix]));
	}

	adixN = nadixv[di];
	if (ad <= adixv[di][0]) {
		adix = 0;
		adw = 1.0;
	} else if (ad >= adixv[di][adixN-1]) {
		adix = adixN-2;
		adw = 0.0;
	} else {
		for (adix = 0; adix < adixN; adix++) {
			if (ad >= adixv[di][adix] && ad <= adixv[di][adix+1])
				break;
		}
		adw = 1.0 - (log(ad) - log(adixv[di][adix]))
		           /(log(adixv[di][adix+1]) - log(adixv[di][adix]));
	}

	/* Lookup & interpolate the log smoothness factor */
//printf("~1 di = %d, ncix = %d, adix = %d\n",di,ncix,adix);
	lsm  = smf[di][ncix][adix]    * ncw          * adw;
	lsm += smf[di][ncix][adix+1]  * ncw          * (1.0 - adw);
	lsm += smf[di][ncix+1][adix]  * (1.0 - ncw)  * adw;
	lsm += smf[di][ncix+1][adix+1] * (1.0 - ncw) * (1.0 - adw);

//printf("~1 lsm = %f\n",lsm);

	for (tweakf = 0.0, i = 1; i < 21; i++)
		tweakf += tweak[i];
	tweakf *= tweak[0];

	sm = pow(10.0, lsm * tweakf);

	/* and correct for the real world with a final tweak table */
	sm *= rwf[di];

//printf("Got log smth %f, returning %1.9f from ncix %d, ncw %f, adix %d, adw %f\n", lsm, sm, ncix, ncw, adix, adw);
	return sm;
}

/* - - - - - - - - - - - - - - - - - - - - - - - -*/
/* Multi-grid temp structure (mgtmp) routines */

/* Create a new mgtmp. */
/* Solution matricies will be NULL */
static mgtmp *new_mgtmp(
	rspl *s,		/* associated rspl */
	int gres[MXDI],	/* resolution to create */
	int f			/* output dimension */
) {
	mgtmp *m;
	int di = s->di;
	int dno = s->d.no;
	int gno, nigc;
	int gres_1[MXDI];
	int e, g, n, i;

	/* Allocate a structure */
	if ((m = (mgtmp *) calloc(1, sizeof(mgtmp))) == NULL)
		error("rspl: malloc failed - mgtmp");

	/* General stuff */
	m->s = s;
	m->f = f;

	/* Grid related */
	for (gno = 1, e = 0; e < di; gno *= gres[e], e++)
		;
	m->g.no = gno;

	/* record high, low limits, and width of each grid cell */
	m->g.mres = 1.0;
	m->g.bres = 0;
	for (e = 0; e < s->di; e++) {
		m->g.res[e] = gres[e];
		gres_1[e] = gres[e] - 1;
		m->g.mres *= gres[e];
		if (gres[e] > m->g.bres) {
			m->g.bres = gres[e];
			m->g.brix = e;
		}
		m->g.l[e] = s->g.l[e];
		m->g.h[e] = s->g.h[e];
		m->g.w[e] = (s->g.h[e] - s->g.l[e])/(double)(gres[e]-1);
	}
	m->g.mres = pow(m->g.mres, 1.0/e);		/* geometric mean */

	/* Compute index coordinate increments into linear grid for each dimension */
	/* ie. 1, gres, gres^2, gres^3 */
	for (m->g.ci[0] = 1, e = 1; e < di; e++)
		m->g.ci[e]  = m->g.ci[e-1] * gres[e-1];		/* In grid points */

	/* Compute index offsets from base of cube to other corners */
	for (m->g.hi[0] = 0, e = 0, g = 1; e < di; g *= 2, e++) {
		for (i = 0; i < g; i++)
			m->g.hi[g+i] = m->g.hi[i] + m->g.ci[e];		/* In grid points */
	}

	/* Number grid cells that contribute to smoothness error */
	for (nigc = 1, e = 0; e < di; e++) {
		nigc *= gres[e]-2;
	}

	/* Downsample ipos arrays */
	for (e = 0; e < s->di; e++) {
		if (s->g.ipos[e] != NULL) {
			unsigned int ix;
			double val, w;
			double inputEnt_1 = (double)(s->g.res[e]-1);
			double inputEnt_2 = (double)(s->g.res[e]-2);

			if ((m->g.ipos[e] = (double *)calloc(m->g.res[e], sizeof(double))) == NULL)
				error("scat: malloc failed - ipos[]");

			/* Compute each downsampled position using linear interpolation */
			for (n = 0; n < m->g.res[e]; n++) { 
				double in = (double)n/(m->g.res[e]-1);
		
				val = in * inputEnt_1;
				if (val < 0.0)
					val = 0.0;
				else if (val > inputEnt_1)
					val = inputEnt_1;
				ix = (unsigned int)floor(val);		/* Coordinate */
				if (ix > inputEnt_2)
					ix = inputEnt_2;
				w = val - (double)ix;		/* weight */
				val = s->g.ipos[e][ix];
				m->g.ipos[e][n] = val + w * (s->g.ipos[e][ix+1] - val);
			}
		}
	}

	/* Compute curvature weighting for matching intermediate resolutions for */
	/* the number of grid points curvature that is accuumulated, as well as the */
	/* geometric effects of a finer fit to the target surface. */
	/* This is all to keep the ratio of sum of smoothness error squared */
	/* constant in relationship to the sum of data point error squared. */
	for (e = 0; e < di; e++) {
		double rsm;				/* Resolution smoothness factor */
		double smooth;

		if (s->symdom)
			rsm = m->g.res[e];	/* Relative final grid size  */
		else
			rsm = m->g.mres;	/* Relative mean final grid size */

		/* Compensate for geometric and grid numeric factors */
		rsm = pow((rsm-1.0), 4.0);	/* Geometric resolution factor for smooth surfaces */
									/* (is ^2 for res. * ^2 with error squared) */
		rsm /= nigc;				/* Average squared non-smoothness */

		/* 2 pass smoothing */
		if (s->tpsm) {
			double lsm;

			lsm = -6.0;
			if (s->tpsm2 != 0)			/* Two pass smoothing second pass */
				lsm += 2.0;				/* Use 100 times the smoothness */
			m->sf.cw[e] = pow(10.0, lsm) * rsm;

		/* Normal */
		} else {

			if (s->smooth >= 0.0) {
				/* Table lookup for optimum smoothing factor */
				smooth = opt_smooth(s, di, s->d.no, s->avgdev[f]);
				m->sf.cw[e] = s->smooth * smooth * rsm;
			} else {	/* Special used to calibrate table */
				m->sf.cw[e] = -s->smooth * rsm;
			}
		}
	}

	/* Compute weighting for weak default function grid value */
	/* We are trying to keep the effect of the wdf constant with */
	/* changes in grid resolution and dimensionality. */
	m->wdfw = s->weak * WEAKW/(m->g.no * (double)di);

	/* Allocate space for auiliary data point related info */
	if ((m->d = (struct mgdat *) calloc(dno, sizeof(struct mgdat))) == NULL)
		error("rspl: malloc failed - mgtmp");

	/* fill in the aux data point info */
	/* (We're assuming N-linear interpolation here. */
	/*  Perhaps we should try simplex too ?) */
	for (n = 0; n < dno; n++) {
		double we[MXRI];	/* 1.0 - Weight in each dimension */
		int ix = 0;			/* Index to base corner of surrounding cube in grid points */

		/* Figure out which grid cell the point falls into */
		for (e = 0; e < di; e++) {
			double t;
			int mi;
			if (s->d.a[n].p[e] < m->g.l[e] || s->d.a[n].p[e] > m->g.h[e]) {
				error("rspl: Data point %d outside grid %e <= %e <= %e",
				                            n,m->g.l[e],s->d.a[n].p[e],m->g.h[e]);
			}
			t = (s->d.a[n].p[e] - m->g.l[e])/m->g.w[e];
			mi = (int)floor(t);			/* Grid coordinate */
			if (mi < 0)					/* Limit to valid cube base index range */
				mi = 0;
			else if (mi >= gres_1[e])	/* Make sure outer point can't be base */
				mi = gres_1[e]-1;
			ix += mi * m->g.ci[e];		/* Add Index offset for grid cube base in dimen */
			we[e] = t - (double)mi;		/* 1.0 - weight */
		}
		m->d[n].b = ix;

		/* Compute corner weights needed for interpolation */
		m->d[n].w[0] = 1.0;
		for (e = 0, g = 1; e < di; g *= 2, e++) {
			for (i = 0; i < g; i++) {
				m->d[n].w[g+i] = m->d[n].w[i] * we[e];
				m->d[n].w[i] *= (1.0 - we[e]);
			}
		}

#ifdef DEBUG
		printf("Data point %d weighting factors = \n",n);
		for (e = 0; e < (1 << di); e++) {
			printf("%d: %f\n",e,m->d[n].w[e]);
		}
#endif /* DEBUG */
	}

	/* Set the solution matricies to unalocated */
	m->q.ccv = NULL;
	m->q.A = NULL;
	m->q.ixcol = NULL;
	m->q.b = NULL;
	m->q.x = NULL;

	return m;
}

/* Completely free an mgtmp */
static void free_mgtmp(mgtmp  *m) {
	int e, di = m->s->di, gno = m->g.no;

	for (e = 0; e < m->s->di; e++) {
		if (m->g.ipos[e] != NULL)
			free(m->g.ipos[e]);
	}
	if (m->q.ccv != NULL)
		free_dmatrix(m->q.ccv,0,gno-1,0,di-1);
	free_dvector(m->q.x,0,gno-1);
	free_dvector(m->q.b,0,gno-1);
	free((void *)m->q.ixcol);
	free_dmatrix(m->q.A,0,gno-1,0,m->q.acols-1);
	free((void *)m->d);
	free((void *)m);
}

/* Initialise the A[][] and b[] matricies ready to solve, given f */
/* (Can be used to re-initialize an mgtmp for changing curve/extra fit factors) */
/* We are setting up the matrix equation Ax = b to solve, where the aim is */
/* to solve the energy minimization problem by setting up a series of interconnected */
/* equations for each grid node value (x) in which the partial derivative */
/* of the equation to be minimized is zero. The A matrix holds the dependence on */
/* the grid points with regard to smoothness and interpolation */
/* fit to the scattered data points, while b holds constant values (e.g. the data */
/* point determined boundary conditions). A[][] stores the packed sparse triangular matrix. */ 

/*

	The overall equation to be minimized is:

		  sum(curvature errors at each grid point) ^ 2
		+ sum(data interpolation errors) ^ 2

	The way this is solved is to take the partial derivative of
	the above with respect to each grid point value, and simultaineously
	solve all the partial derivative equations for zero.

	Each row of A[][] and b[] represents the cooeficients of one of
	the partial derivative equations (it does NOT correspond to one
	grid points curvature etc.). Because the partial derivative
	of any sum term that does not have the grid point in question in it
	will have a partial derivative of zero, each row equation consists
	of just those terms that have that grid points value in it,
	with the vast majoroty of the sum terms omitted.

 */

static void setup_solve(
	mgtmp  *m,		/* initialized grid temp structure */
	int final		/* nz if final resolution (activate EXTRA_SURFACE_SMOOTHING) */
) {
	rspl *s = m->s;
	int di   = s->di;
	int gno  = m->g.no,   dno = s->d.no;
	int *gres = m->g.res, *gci = m->g.ci;
	int f = m->f;				/* Output dimensions being worked on */

	double **ccv = m->q.ccv;	/* ccv vector for adjusting simultabeous equation */
	double **A  = m->q.A;		/* A matrix of interpoint weights */
	int acols   = m->q.acols;	/* A matrix packed columns needed */
	int *xcol   = m->q.xcol;		/* A array column translation from packed to sparse index */ 
	int *ixcol  = m->q.ixcol;	/* A array column translation from sparse to packed index */ 
	double *b   = m->q.b;		/* b vector for RHS of simultabeous equation */
	double *x   = m->q.x;		/* x vector for LHS of simultabeous equation */
	int e, n,i,k;
	double oawt;				/* Overall adjustment weight */
	double nbsum;				/* normb sum */

//printf("~1 setup_solve got ccv = 0x%x\n",ccv);

	/* Allocate and init the A array column sparse packing lookup and inverse. */
	/* Note that this only works for a minumum grid resolution of 4. */
	/* The sparse di dimension region allowed for is a +/-1 cube around the point */
	/* question, plus +/-2 offsets in axis direction only, */
	/* plus +/-3 offset in axis directions if 2nd order smoothing is defined. */
	if (A == NULL) {			/* Not been allocated previously */
		DCOUNT(gc, MXDIDO, di, -3, -3, 4);	/* Step through +/- 3 cube offset */
		int ix;						/* Grid point offset in grid points */
		acols = 0;
	
		DC_INIT(gc);
	
		while (!DC_DONE(gc)) {
			int n3 = 0, n2 = 0, nz = 0;
	
			/* Detect +/-3 +/-2 and 0 elements */
			for (k = 0; k < di; k++) {
				if (gc[k] == 3 || gc[k] == -3)
					n3++;
				if (gc[k] == 2 || gc[k] == -2)
					n2++;
				if (gc[k] == 0)
					nz++;
			}

			/* Accept only if doesn't have a +/-2, */
			/* or if it has exactly one +/-2 and otherwise 0 */
			if ((n3 == 0 && n2 == 0)
			 || (n2 == 1 && nz == (di-1))
#ifdef SMOOTH2
			 || (n3 == 1 && nz == (di-1))
#endif /* SMOOTH2*/
			  ) {
				for (ix = 0, k = 0; k < di; k++)
					ix += gc[k] * gci[k];		/* Multi-dimension grid offset */
				if (ix >= 0) {
					xcol[acols++] = ix;			/* We only store half, due to symetry */
				}
			}
			DC_INC(gc);
		}

		ix = xcol[acols-1] + 1;	/* Number of expanded rows */

		/* Create inverse lookup */
		if (ixcol == NULL) {
			if ((ixcol = (int *) malloc(ix * sizeof(int))) == NULL)
				error("rspl malloc failed - ixcol");
		}

		for (k = 0; k < ix; k++)
			ixcol[k] = -0x7fffffff;	/* Mark rows that aren't allowed for */

		for (k = 0; k < acols; k++)
			ixcol[xcol[k]] = k;		/* Set inverse lookup */

#ifdef DEBUG
		printf("Sparse array expansion = \n");
		for (k = 0; k < acols; k++) {
			printf("%d: %d\n",k,xcol[k]);
		}
		printf("\nSparse array encoding = \n");
		for (k = 0; k < ix; k++) {
			printf("%d: %d\n",k,ixcol[k]);
		}
#endif /* DEBUG */

		/* We store the packed diagonals of the sparse A matrix */
		/* If re-initializing, zero matrices, else allocate zero'd matricies */
		if ((A = dmatrixz(0,gno-1,0,acols-1)) == NULL) {
			error("Malloc of A[][] failed with [%d][%d]",gno,acols);
		}
		if ((b = dvectorz(0,gno)) == NULL) {
			free_dmatrix(A,0,gno-1,0,acols-1);
			error("Malloc of b[] failed");
		}
		if ((x = dvector(0,gno-1)) == NULL) {
			free_dmatrix(A,0,gno-1,0,acols-1);
			free_dvector(b,0,gno-1);
			error("Malloc of x[] failed");
		}

		/* Stash in the mgtmp */
		m->q.ccv = ccv;
		m->q.A = A;
		m->q.b = b;
		m->q.x = x;
		m->q.acols = acols;
		m->q.ixcol = ixcol;

	} else { 	/* re-initializing, zero matrices */
		for (i = 0; i < gno; i++)
			for (k = 0; k < acols; k++) {
				A[i][k] = 0.0;
			}
		for (i = 0; i < gno; i++)
			b[i] = 0.0;
	}

#ifdef NEVER
	/* Production version, without extra edge weight */

	/* Accumulate curvature dependent factors to the triangular A matrix. */
	/* Because it's triangular, we compute and add in all the weighting */
	/* factors at and to the right of each cell. */

	/* The ipos[] factor is to allow for the possibility that the */
	/* grid spacing may be non-uniform in the colorspace where the */
	/* function being modelled is smooth. Our curvature computation */
	/* needs to make allowsance for this fact in computing the */
	/* node value differences that equate to zero curvature. */ 
	/*
		The old curvature fixed grid spacing equation was:
			ki * (u[i-1] - 2 * u[i] + u[i+1])^2
		with derivatives wrt each node:
			ki-1 *  1 * 2 * u[i-1] 
			ki   * -2 * 2 * u[i]
			ki+1 *  1 * 2 * u[i+1]

		Allowing for scaling of each grid difference by w[i-1] and w[i],
		where w[i-1] corresponds to the width of cell i-1 to i,
		and w[i] corresponds to the width of cell i to i+1:
			ki * (w[i-1] * (u[i-1] - u[i]) + w[i] * (u[i+1] - u[i[))^2
		=	ki * (w[i-1] * u[i-1] - (w[i-1] + w[i]) * u[i]) + w[i] * u[i+1])^2
		with derivatives wrt each node:
			ki-1 *   w[i-1]         *   w[i-1] * u[i-1]
			ki   * -(w[i-1] + w[i]) * -(w[i-1] + w[i]) * u[i])
			ki+1 *   w[i]           *   w[i] * u[i+1]
	 */

	{		/* Setting this up from scratch */
		ECOUNT(gc, MXDIDO, di, 0, gres, 0);
		EC_INIT(gc);

		for (oawt = 0.0, i = 1; i < 21; i++)
			oawt += wvals[i];
		oawt *= wvals[0];

		for (i = 0; i < gno; i++) {

			for (e = 0; e < di; e++) {	/* For each curvature direction */
				double w0, w1, tt;
				double cw = 2.0 * m->sf.cw[e];	/* Overall curvature weight */
				cw *= s->d.vw[f];				/* Scale curvature weight for data range */

				/* If at least two above lower edge in this dimension */
				/* Add influence on Curvature of cell below */
				if ((gc[e]-2) >= 0) {
					/* double kw = cw * gp[UO_C(e,1)].k; */	/* Cell bellow k value */
					double kw = cw;
					w0 = w1 = 1.0;
					if (m->g.ipos[e] != NULL) {
						w0 = fabs(m->g.ipos[e][gc[e]-1] - m->g.ipos[e][gc[e]-2]);
						w1 = fabs(m->g.ipos[e][gc[e]-0] - m->g.ipos[e][gc[e]-1]);
						tt = sqrt(w0 * w1);		/* Normalise overall width weighting effect */
						w1 = tt/w1;
					}
					A[i][ixcol[0]] += w1 * w1 * kw;
					if (ccv != NULL)
						b[i] += kw * (w1) * ccv[i - gci[e]][e]; /* Curvature compensation value */
				}
				/* If not one from upper or lower edge in this dimension */
				/* Add influence on Curvature of this cell */
				if ((gc[e]-1) >= 0 && (gc[e]+1) < gres[e]) {
					/* double kw = cw * gp->k;  */		/* This cells k value */
					double kw = cw; 
					w0 = w1 = 1.0;
					if (m->g.ipos[e] != NULL) {
						w0 = fabs(m->g.ipos[e][gc[e]-0] - m->g.ipos[e][gc[e]-1]);
						w1 = fabs(m->g.ipos[e][gc[e]+1] - m->g.ipos[e][gc[e]-0]);
						tt = sqrt(w0 * w1);
						w0 = tt/w0;
						w1 = tt/w1;
					}
					A[i][ixcol[0]]      += -(w0 + w1) * -(w0 + w1) * kw;
					A[i][ixcol[gci[e]]] += -(w0 + w1) * w1 * kw * oawt;
					if (ccv != NULL)
						b[i] += kw * -(w0 + w1) * ccv[i][e]; /* Curvature compensation value */
				}
				/* If at least two below the upper edge in this dimension */
				/* Add influence on Curvature of cell above */
				if ((gc[e]+2) < gres[e]) {
					/* double kw = cw * gp[UO_C(e,2)].k;	*/ /* Cell above k value */
					double kw = cw;
					w0 = w1 = 1.0;
					if (m->g.ipos[e] != NULL) {
						w0 = fabs(m->g.ipos[e][gc[e]+1] - m->g.ipos[e][gc[e]+0]);
						w1 = fabs(m->g.ipos[e][gc[e]+2] - m->g.ipos[e][gc[e]+1]);
						tt = sqrt(w0 * w1);
						w0 = tt/w0;
						w1 = tt/w1;
					}
					A[i][ixcol[0]]          += w0 * w0 * kw;
					A[i][ixcol[gci[e]]]     += w0 * -(w0 + w1) * kw;
					A[i][ixcol[2 * gci[e]]] += w0 * w1 * kw;
					if (ccv != NULL)
						b[i] += kw * -(w0 + w1) * ccv[i][e]; /* Curvature compensation value */
				}
			}
			EC_INC(gc);
		}
	}
#endif /* NEVER */

#ifdef ALWAYS
	/* Production version that allows for extra weight on grid edges */

	/* Accumulate curvature dependent factors to the triangular A matrix. */
	/* Because it's triangular, we compute and add in all the weighting */
	/* factors at and to the right of each cell. */

	/* The ipos[] factor is to allow for the possibility that the */
	/* grid spacing may be non-uniform in the colorspace where the */
	/* function being modelled is smooth. Our curvature computation */
	/* needs to make allowsance for this fact in computing the */
	/* node value differences that equate to zero curvature. */ 
	/*
		The old curvature fixed grid spacing equation was:
			ki * (u[i-1] - 2 * u[i] + u[i+1])^2
		with derivatives wrt each node:
			ki-1 *  1 * 2 * u[i-1] 
			ki   * -2 * 2 * u[i]
			ki+1 *  1 * 2 * u[i+1]

		Allowing for scaling of each grid difference by w[i-1] and w[i],
		where w[i-1] corresponds to the width of cell i-1 to i,
		and w[i] corresponds to the width of cell i to i+1:
			ki * (w[i-1] * (u[i-1] - u[i]) + w[i] * (u[i+1] - u[i[))^2
		=	ki * (w[i-1] * u[i-1] - (w[i-1] + w[i]) * u[i]) + w[i] * u[i+1])^2
		with derivatives wrt each node:
			ki-1 *   w[i-1]         *   w[i-1] * u[i-1]
			ki   * -(w[i-1] + w[i]) * -(w[i-1] + w[i]) * u[i])
			ki+1 *   w[i]           *   w[i] * u[i+1]
	 */
	{		/* Setting this up from scratch */
		ECOUNT(gc, MXDIDO, di, 0, gres, 0);
#ifdef EXTRA_SURFACE_SMOOTHING
//		double  k0w = 4.0, k1w = 1.3333;	/* Extra stiffness */
//		double  k0w = 3.0, k1w = 1.26;		/* Some extra stiffness */
		double  k0w = 2.0, k1w = 1.15;		/* A little extra stiffness */
#else
		double  k0w = 1.0, k1w = 1.0;		/* No extra weights */
#endif

		EC_INIT(gc);
		for (oawt = 0.0, i = 1; i < 21; i++)
			oawt += wvals[i];
		oawt *= wvals[0];

		if (final == 0)
			k0w = k1w = 1.0;			/* Activate extra edge smoothing on final grid ? */

		for (i = 0; i < gno; i++) {
			int k;

			/* We're creating the equation cooeficients for solving the */
			/* partial derivative equation w.r.t. node point i. */
			/* Due to symetry in the smoothness interactions, only */
			/* the triangle cooeficients of neighbour nodes is needed. */
			for (e = 0; e < di; e++) {	/* For each curvature direction */
				double kw, w0, w1, tt;
				double cw = 2.0 * m->sf.cw[e];	/* Overall curvature weight */
				double xx = 1.0;				/* Extra edge weighing */
				cw *= s->d.vw[f];				/* Scale curvature weight for data range */

				/* weight factor for outer or 2nd outer in other dimensions */
				for (k = 0; k < di; k++) {
					if (k == e)
						continue;
					if (gc[k] == 0 || gc[k] == (gres[k]-1))
						xx *= k0w;
					else if (gc[k] == 1 || gc[k] == (gres[k]-2))
						xx *= k1w;
				}

				/* If at least two above lower edge in this dimension */
				/* Add influence on Curvature of cell below */
				if ((gc[e]-2) >= 0) {
					/* double kw = cw * gp[-gc[e]].k; */	/* Cell bellow k value */
					kw = cw * xx;
					w0 = w1 = 1.0;
					if (m->g.ipos[e] != NULL) {
						w0 = fabs(m->g.ipos[e][gc[e]-1] - m->g.ipos[e][gc[e]-2]);
						w1 = fabs(m->g.ipos[e][gc[e]-0] - m->g.ipos[e][gc[e]-1]);
						tt = sqrt(w0 * w1);		/* Normalise overall width weighting effect */
						w1 = tt/w1;
					}
					if ((gc[e]-2) == 0 || (gc[e]+0) == (gres[e]-1))
						kw *= k0w;
					else if ((gc[e]-2) == 1 || (gc[e]-0) == (gres[e]-2))
						kw *= k1w;
					A[i][ixcol[0]] += kw * (w1) * w1;
					if (ccv != NULL) {
//printf("~1 tweak b[%d] by %e\n",i,kw * (w1) * ccv[i - gci[e]][e]);
						b[i] += kw * (w1) * ccv[i - gci[e]][e]; /* Curvature compensation value */
					}
				}
				/* If not one from upper or lower edge in this dimension */
				/* Add influence on Curvature of this cell */
				if ((gc[e]-1) >= 0 && (gc[e]+1) < gres[e]) {
					/* double kw = cw * gp->k;  */		/* This cells k value */
					kw = cw * xx;
					w0 = w1 = 1.0;
					if (m->g.ipos[e] != NULL) {
						w0 = fabs(m->g.ipos[e][gc[e]-0] - m->g.ipos[e][gc[e]-1]);
						w1 = fabs(m->g.ipos[e][gc[e]+1] - m->g.ipos[e][gc[e]-0]);
						tt = sqrt(w0 * w1);
						w0 = tt/w0;
						w1 = tt/w1;
					}
					if ((gc[e]-1) == 0 || (gc[e]+1) == (gres[e]-1))
						kw *= k0w;
					else if ((gc[e]-1) == 1 || (gc[e]+1) == (gres[e]-2))
						kw *= k1w;
					A[i][ixcol[0]]      += kw * -(w0 + w1) * -(w0 + w1);
					A[i][ixcol[gci[e]]] += kw * -(w0 + w1) * w1 * oawt;
					if (ccv != NULL) {
//printf("~1 tweak b[%d] by %e\n",i, kw * -(w0 + w1) * ccv[i][e]);
						b[i] += kw * -(w0 + w1) * ccv[i][e]; /* Curvature compensation value */
					}
				}
				/* If at least two below the upper edge in this dimension */
				/* Add influence on Curvature of cell above */
				if ((gc[e]+2) < gres[e]) {
					/* double kw = cw * gp[gc[e]].k;	*/ /* Cell above k value */
					kw = cw * xx;
					w0 = w1 = 1.0;
					if (m->g.ipos[e] != NULL) {
						w0 = fabs(m->g.ipos[e][gc[e]+1] - m->g.ipos[e][gc[e]+0]);
						w1 = fabs(m->g.ipos[e][gc[e]+2] - m->g.ipos[e][gc[e]+1]);
						tt = sqrt(w0 * w1);
						w0 = tt/w0;
						w1 = tt/w1;
					}
					if ((gc[e]+0) == 0 || (gc[e]+2) == (gres[e]-1))
						kw *= k0w;
					else if ((gc[e]+0) == 1 || (gc[e]+2) == (gres[e]-2))
						kw *= k1w;
					A[i][ixcol[0]]          += kw * (w0) * w0;
					A[i][ixcol[gci[e]]]     += kw * (w0) * -(w0 + w1);
					A[i][ixcol[2 * gci[e]]] += kw * (w0) * w1;
					if (ccv != NULL) {
//printf("~1 tweak b[%d] by %e\n",i, kw * (w0) * ccv[i + gci[e]][e]);
						b[i] += kw * (w0) * ccv[i + gci[e]][e]; /* Curvature compensation value */
					}
				}
			}
			EC_INC(gc);
		}
	}
#endif /* ALWAYS */

#ifdef DEBUG
	printf("After adding curvature equations:\n");
	for (i = 0; i < gno; i++) {
		printf("b[%d] = %f\n",i,b[i]);
		for (k = 0; k < acols; k++) {
			printf("A[%d][%d] = %f\n",i,k,A[i][k]);
		}
		printf("\n");
	}
#endif /* DEBUG */

	nbsum = 0.0;	/* Zero sum of b[] squared */

#ifdef ALWAYS
	/* Accumulate weak default function factors. These are effectively a */
	/* weak "data point" exactly at each grid point. */
	/* (Note we're not currently doing this in a cache friendly order,   */
	/*  and we're calling the function once for each output component..) */
	if (s->dfunc != NULL) {		/* Setting this up from scratch */
		double iv[MXDI], ov[MXDO];
		ECOUNT(gc, MXDIDO, di, 0, gres, 0);
		EC_INIT(gc);
		for (i = 0; i < gno; i++) {
			double d, tt;

			/* Get weak default function value for this grid point */
			for (e = 0; e < s->di; e++)
				iv[e] = m->g.l[e] + gc[e] * m->g.w[e];	/* Input sample values */
			s->dfunc(s->dfctx, ov, iv);

			/* Compute values added to matrix */
			d = 2.0 * m->wdfw;
			tt = d * ov[f];			/* Change in data component */
			nbsum += (2.0 * b[i] + tt) * tt;	/* += (b[i] + tt)^2 - b[i]^2 */
			b[i] += tt;				/* New data component value */
			A[i][0] += d;			/* dui component to itself */

			EC_INC(gc);
		}

#ifdef DEBUG
		printf("After adding weak default equations:\n");
		for (i = 0; i < gno; i++) {
			printf("b[%d] = %f\n",i,b[i]);
			for (k = 0; k < acols; k++) {
				printf("A[%d][%d] = %f\n",i,k,A[i][k]);
			}
			printf("\n");
		}
#endif /* DEBUG */

	}
#endif /* ALWAYS */

#ifdef ALWAYS
	/* Accumulate data point dependent factors */
	for (n = 0; n < dno; n++) {		/* Go through all the data points */
		int j,k;
		int bp = m->d[n].b; 		/* index to base grid point in grid points */

		/* For each point in the cube as the base grid point, */
		/* add in the appropriate weighting for its weighted neighbors. */
		for (j = 0; j < (1 << di); j++) {	/* Binary sequence */
			double d, w, tt;
			int ai;

			ai = bp + m->g.hi[j];			/* A matrix index */

			w = m->d[n].w[j];				/* Base point grid weight */
			d = 2.0 * s->d.a[n].k[f] * w;	/* (2.0, w are derivative factors, k data pnt wgt) */
			tt = d * s->d.a[n].cv[f];		/* Change in (corrected) data component */

			nbsum += (2.0 * b[ai] + tt) * tt;	/* += (b[ai] + tt)^2 - b[ai]^2 */
			b[ai] += tt;						/* New data component value */
			A[ai][0] += d * w;					/* dui component to itself */

			/* For all the other simplex points ahead of this one, */
			/* add in linear interpolation derivative weightings */
			for (k = j+1; k < (1 << di); k++) {	/* Binary sequence */
				int ii;
				ii = ixcol[m->g.hi[k] - m->g.hi[j]];	/* A matrix column index */
				A[ai][ii] += d * m->d[n].w[k];			/* dui component due to ui+1 */
			}
		}
	}

	/* Compute norm of b[] from sum of squares */
	nbsum = sqrt(nbsum);
	if (nbsum < 1e-4) 
		nbsum = 1e-4;
	m->q.normb = nbsum;

#endif /* ALWAYS */ 

#ifdef DEBUG
	printf("After adding data point equations:\n");
	for (i = 0; i < gno; i++) {
		printf("b[%d] = %f\n",i,b[i]);
		for (k = 0; k < acols; k++) {
			printf("A[%d][%d] = %f\n",i,k,A[i][k]);
		}
		printf("\n");
	}
#endif /* DEBUG */

//	exit(0);
}


/* Given that we've done a complete fit at the current resolution, */
/* allocate and compute the curvature error of each grid point and put it in */
/* s->g.ccv[gno][di] */
static void comp_ccv(
	mgtmp  *m		/* Solution to use */
) {
	rspl *s = m->s;
	int gno = m->g.no, *gres = m->g.res, *gci = m->g.ci;
	int di = s->di;
	double *x = m->q.x;		/* Grid solution values */
	int f = m->f;			/* Output dimensions being worked on */
	int e, i;

	ECOUNT(gc, MXDIDO, di, 0, gres, 0);
	EC_INIT(gc);

	if (s->g.ccv == NULL) {
		if ((s->g.ccv = dmatrixz(0, gno-1, 0, di-1)) == NULL) {
			error("Malloc of ccv[] failed with [%d][%d]",di,gno);
		}
	}

	for (i = 0; i < gno; i++) {
		for (e = 0; e < di; e++) {	/* For each curvature direction */
			double w0, w1, tt;

			s->g.ccv[i][e] = 0.0;		/* Default value */

			/* If not one from upper or lower edge in this dimension */
			if ((gc[e]-1) >= 0 && (gc[e]+1) < gres[e]) {
				/* double kw = cw * gp->k;  */		/* This cells k value */
				w0 = w1 = 1.0;
				if (m->g.ipos[e] != NULL) {
					w0 = fabs(m->g.ipos[e][gc[e]-0] - m->g.ipos[e][gc[e]-1]);
					w1 = fabs(m->g.ipos[e][gc[e]+1] - m->g.ipos[e][gc[e]-0]);
					tt = sqrt(w0 * w1);
					w0 = tt/w0;
					w1 = tt/w1;
				}
				s->g.ccv[i][e] +=   w0       * x[i - gci[e]];
				s->g.ccv[i][e] += -(w0 + w1) * x[i];
				s->g.ccv[i][e] +=   w1       * x[i + gci[e]];
			}
//printf("~1 computing ccv for node %d is %f\n",i,s->g.ccv[i][0]);
		}
		EC_INC(gc);
	}
}

/* Down sample the curvature compensation values in s->g.ccv to */
/* a given solution. Allocate the m->q.ccv if necessary. */
static void init_ccv(
	mgtmp  *m		/* Destination */
) {
	rspl *s = m->s;
	int f = m->f;			/* Output dimensions being worked on */
	int di  = s->di;
	int gno = m->g.no;
	int gres1_1[MXDI];	/* Destination */
	int gres2_1[MXDI];	/* Source */
	double scale[MXDI];	/* ccv scale factor */
	int e, n;
	ECOUNT(gc, MXDIDO, di, 0, m->g.res, 0);	/* Counter for output points */

	for (e = 0; e < di; e++) {
		gres1_1[e] = m->g.res[e]-1;
		gres2_1[e] = s->g.res[e]-1;
	}

	if (m->q.ccv == NULL) {
		if ((m->q.ccv = dmatrixz(0, gno-1, 0, di-1)) == NULL) {
			error("Malloc of ccv[] failed with [%d][%d]",di,gno);
		}
	}

	/* Compute the scale factor to compensate for the grid resolution */
	/* effect on the grid difference values. */
	for (e = 0; e < di; e++) { 
		double rsm_s, rsm_d;

		if (s->symdom) {			/* Relative final grid size  */
			rsm_s = s->g.res[e];
			rsm_d = m->g.res[e];
		} else {					/* Relative mean final grid size */
			rsm_s = s->g.mres;
			rsm_d = m->g.mres;
		}
	
		rsm_s = pow((rsm_s-1.0), 2.0);	/* Geometric resolution factor for smooth surfaces */
		rsm_d = pow((rsm_d-1.0), 2.0);	/* (It's ^2 rather than ^4 as it's is before squaring) */

		scale[e] = rsm_s/rsm_d;
	}

	/* Point sampling is probably not the ideal way of down sampling, */
	/* but it's easy, and won't be too bad if the s->g.ccv has been */
	/* low pass filtered. */

	/* For all grid ccv's */
	EC_INIT(gc);
	for (n = 0; n < gno; n++) {
		double we[MXRI];		/* 1.0 - Weight in each dimension */
		double gw[POW2MXRI];	/* weight for each grid cube corner */
		int ix;					/* Index of source ccv grid cube base */
		
		/* Figure out which grid cell the point falls into */
		{
			double t;
			int mi;
			ix = 0;
			for (e = 0; e < di; e++) {
				t = ((double)gc[e]/(double)gres1_1[e]) * (double)gres2_1[e];
				mi = (int)floor(t);			/* Grid coordinate */
				if (mi < 0)					/* Limit to valid cube base index range */
					mi = 0;
				else if (mi >= gres2_1[e])
					mi = gres2_1[e]-1;
				ix += mi * s->g.ci[e];		/* Add Index offset for grid cube base in dimen */
				we[e] = t - (double)mi;		/* 1.0 - weight */
			}
		}

		/* Compute corner weights needed for interpolation */
		{
			int i, g;
			gw[0] = 1.0;
			for (e = 0, g = 1; e < di; g *= 2, e++) {
				for (i = 0; i < g; i++) {
					gw[g+i] = gw[i] * we[e];
					gw[i] *= (1.0 - we[e]);
				}
			}
		}

		/* Compute the output values */
		{
			int i;
			for (e = 0; e < di; e++) 
				m->q.ccv[n][e] = 0.0;			/* Zero output value */
			for (i = 0; i < (1 << di); i++) {	/* For all corners of cube */
				int oix = ix + s->g.hi[i];
				for (e = 0; e < di; e++) 
					m->q.ccv[n][e] += gw[i] * s->g.ccv[oix][e];
			}
			/* Rescale curvature for grid spacing */
			for (e = 0; e < di; e++) 
				m->q.ccv[n][e] *= scale[e];
//printf("~1 downsampling ccv for node %d is %f\n",n,m->q.ccv[n][0]);
		}
		EC_INC(gc);
	}
}

/* Apply a gaussian filter to the curvature compensation values */
/* s->g.ccv[gno][di], to apply smoothing. */
static void filter_ccv(
	rspl *s,
	double stdev		/* Standard deviation diameter of filter (in input) */
						/* 1.0 = grid width */
) {
	int k, e, ee, i, j, di = s->di;
	int gno = s->g.no, *gres = s->g.res, *gci = s->g.ci;
	double *_fkern[MXDI], *fkern[MXDI];		/* Filter kernels */
	int kmin[MXDI], kmax[MXDI];				/* Kernel index range (inclusive) */
	double *_row, *row;				/* Extended copy of each row processed */

//printf("Doing filter stdev %f\n",stdev);

//printf("~1 bres = %d, index %d to %d\n",s->g.bres,-s->g.bres+1,s->g.bres+s->g.bres-1);
	if ((_row = (double *) malloc(sizeof(double) * (s->g.bres * 3 - 2))) == NULL)
		error("rspl malloc failed - ccv row copy");
	row = _row + s->g.bres-1;	/* Allow +/- gres-1 */

	/* Compute the kernel weightings for the given stdev */
	for (ee = 0; ee < di; ee++) {		/* For each dimension direction */
		int cres;						/* Current res */
		double k1, k2, tot;

//printf("Filter along dim %d:\n",ee);
		if ((_fkern[ee] = (double *) malloc(sizeof(double) * (gres[ee] * 2 - 1))) == NULL)
			error("rspl malloc failed - ccv filter kernel");
		fkern[ee] = _fkern[ee] + gres[ee]-1;	/* node of interest at center */

		/* Take gaussian constants out of the loop */
		k2 = 1.0 / (2.0 * pow(fabs(stdev), TWOPASSORDER));
		k1 = k2 / 3.1415926;

		/* Comute the range needed */
		if (s->symdom) {
			cres = gres[ee];
		} else {
			cres = s->g.mres;
		}
		kmin[ee] = (int)floor(-5.0 * stdev * (cres-1.0));
		kmax[ee] = (int)ceil(5.0 * stdev * (cres-1.0));

		if (kmin[ee] < (-gres[ee]+1))
			kmin[ee] = -gres[ee]+1;
		else if (kmin[ee] > -1)
			kmin[ee] = -1;
		if (kmax[ee] > (gres[ee]-1))
			kmax[ee] = gres[ee]-1;
		else if (kmax[ee] < 1)
			kmax[ee] = 1;
//printf("kmin = %d, kmax = %d\n",kmin[ee], kmax[ee]);

		for (tot = 0.0, i = kmin[ee]; i <= kmax[ee]; i++) {
			double fi = (double)i;

			/* Do a discrete integration of the gassian function */
			/* to compute discrete weightings */
			fkern[ee][i] = 0.0;
			for (k = -4; k < 5; k++) {
				double oset = (fi + k/9.0)/(cres-1.0);
				double val;

				val = k1 * exp(-k2 * pow(fabs(oset), TWOPASSORDER));
				fkern[ee][i] += val;
				tot += val;
			}
		}
		/* Normalize the sum */
		for (tot = 1.0/tot, i = kmin[ee]; i <= kmax[ee]; i++)
			fkern[ee][i] *= tot;
//printf("Filter cooefs:\n");
//for (i = kmin[ee]; i <= kmax[ee]; i++)
//printf("%d: %e\n",i,fkern[ee][i]);
	}

	for (k = 0; k < di; k++) {			/* For each curvature direction */
		for (ee = 0; ee < di; ee++) {	/* For each dimension direction */
			int tgres[MXDI-1];
			
//printf("~1 Filtering curv dir %d, dim dir %d\n",k,ee);
			/* Setup counters for scanning through all other dimensions */
			for (j = e = 0; e < di; e++) {
				if (e == ee)
					continue;
				tgres[j++] = gres[e];
			}
			/* For each row of this dimension */
			{
				ECOUNT(gc, MXDIDO-1, di-1, 0, tgres, 0);	/* Count other dimensions */

				EC_INIT(gc);
				for (; di <= 1 || !EC_DONE(gc);) {
					int ix;

					/* Compute index of start of row */
					for (ix = j = e = 0; e < di; e++) {
						if (e == ee)
							continue;
						ix += gc[j++] * gci[e]; 
					}

					/* Copy row to temporary array, and expand */
					/* edge values by mirroring them. */
					for (i = 0; i < gres[ee]; i++)
						row[i] = s->g.ccv[ix + i * gci[ee]][k];
					for (i = kmin[ee]; i < 0; i++)
						row[i] = 2.0 * row[0] - row[-i];	/* Mirror the value */
					for (i = gres[ee]-1 + kmax[ee]; i > (gres[ee]-1); i--)
						row[i] = 2.0 * row[gres[ee]-1] - row[gres[ee]-1-i];	/* Mirror the value */
//printf("~1 Row = \n");
//for (i = kmin[ee]; i <= (gres[ee]-1 + kmax[ee]); i++)
//printf("%d: %f\n",i,row[i]);
		
					/* Apply the 1D convolution to the temporary array */
					/* to produce the filtered values. */
					for (i = 0; i < gres[ee]; i++) {
						double fv;
						
						for (fv = 0.0, j = kmin[ee]; j <= kmax[ee]; j++)
							fv += fkern[ee][j] * row[i + j];
						s->g.ccv[ix + i * gci[ee]][k] = fv;
					}
					if (di <= 1)
						break;
					EC_INC(gc);
				}
			}
		}
	}

	for (ee = 0; ee < di; ee++)
		free(_fkern[ee] );
	free(_row);
}

/* Given that we've done a complete fit at the current resolution, */
/* compute the error of each data point, and then compute */
/* a correction factor .cv[] for each point from this. */
static void comp_extrafit_corr(
	mgtmp *m		/* Current resolution mgtmp */
) {
	rspl *s = m->s;
	int n;
	int dno = s->d.no;
	int di = s->di;
	double *x = m->q.x;		/* Grid solution values */
	int f = m->f;			/* Output dimensions being worked on */

	/* Compute error for each data point */
	for (n = 0; n < dno; n++) {
		int j;
		int bp = m->d[n].b; 		/* index to base grid point in grid points */
		double val;					/* Current interpolated value */
		double err;
		double gain = 1.0;

		/* Compute the interpolated grid value for this data point */
		for (val = 0.0, j = 0; j < (1 << di); j++) {	/* Binary sequence */
			val += m->d[n].w[j] * x[bp + m->g.hi[j]];
		}

		err = s->d.a[n].v[f] - val;

#ifdef NEVER
		/* Compute gain from previous move */
		if (fabs(s->d.a[n].pe[f]) > 0.001) {
			gain = (val - s->d.a[n].pv[f])/s->d.a[n].pe[f];
			if (gain < 0.2)
				gain = 0.2;
			else if (gain > 5.0)
				gain = 5.0;
			gain = pow(gain, 0.6);
		} else {
			gain = 1.0;
		}
#endif
		/* Correct the target data point value by the error */
		s->d.a[n].cv[f] += err / gain;

//printf("~1 Data point %d, v = %f, cv = %f, change = %f\n",n,s->d.a[n].v[f],s->d.a[n].cv[f],-val);
//printf("~1 Data point %d, pe = %f, change = %f, gain = %f\n",n,s->d.a[n].pe[f],val - s->d.a[n].pv[f],gain);
//printf("~1 Data point %d err = %f, target %f, was %f, now %f\n",n,err,s->d.a[n].v[f],val,s->d.a[n].cv[f]);
//		s->d.a[n].pe[f] = err / gain;
//		s->d.a[n].pv[f] = val;
	}
}

/* Transfer a solution from one mgtmp to another */
/* (We assume that they are for the same problem) */
static void init_soln(
	mgtmp  *m1,		/* Destination */
	mgtmp  *m2		/* Source */
) {
	rspl *s = m1->s;
	int di  = s->di;
	int gno = m1->g.no;
	int gres1_1[MXDI];
	int gres2_1[MXDI];
	int e, n;
	ECOUNT(gc, MXDIDO, di, 0, m1->g.res, 0);	/* Counter for output points */

	for (e = 0; e < di; e++) {
		gres1_1[e] = m1->g.res[e]-1;
		gres2_1[e] = m2->g.res[e]-1;
	}

	/* For all output grid points */
	EC_INIT(gc);
	for (n = 0; n < gno; n++) {
		double we[MXRI];		/* 1.0 - Weight in each dimension */
		double gw[POW2MXRI];	/* weight for each grid cube corner */
		double *gp;				/* Pointer to x2[] grid cube base */
		
		/* Figure out which grid cell the point falls into */
		{
			double t;
			int mi;
			gp = m2->q.x;					/* Base of solution array */
			for (e = 0; e < di; e++) {
				t = ((double)gc[e]/(double)gres1_1[e]) * (double)gres2_1[e];
				mi = (int)floor(t);			/* Grid coordinate */
				if (mi < 0)					/* Limit to valid cube base index range */
					mi = 0;
				else if (mi >= gres2_1[e])
					mi = gres2_1[e]-1;
				gp += mi * m2->g.ci[e];		/* Add Index offset for grid cube base in dimen */
				we[e] = t - (double)mi;		/* 1.0 - weight */
			}
		}

		/* Compute corner weights needed for interpolation */
		{
			int i, g;
			gw[0] = 1.0;
			for (e = 0, g = 1; e < di; g *= 2, e++) {
				for (i = 0; i < g; i++) {
					gw[g+i] = gw[i] * we[e];
					gw[i] *= (1.0 - we[e]);
				}
			}
		}

		/* Compute the output values */
		{
			int i;
			m1->q.x[n] = 0.0;					/* Zero output value */
			for (i = 0; i < (1 << di); i++) {	/* For all corners of cube */
				m1->q.x[n] += gw[i] * gp[m2->g.hi[i]];
			}
		}
		EC_INC(gc);
	}
}

/* - - - - - - - - - - - - - - - - - - - -*/

static double one_itter1(cj_arrays *ta, double **A, double *x, double *b, double normb,
                         int gno, int acols, int *xcol, int di, int *gres, int *gci,
                         int max_it, double tol);
static void one_itter2(double **A, double *x, double *b, int gno, int acols, int *xcol,
                 int di, int *gres, int *gci, double ovsh);
static double soln_err(double **A, double *x, double *b, double normb, int gno, int acols, int *xcol);
static double cj_line(cj_arrays *ta, double **A, double *x, double *b, int gno, int acols,
                      int *xcol, int sof, int nid, int inc, int max_it, double tol);

/* Solve scattered data to grid point fit */
static void
solve_gres(mgtmp *m, cj_arrays *ta, double tol, int final)
{
	rspl *s = m->s;
	int di = s->di;
	int gno = m->g.no, *gres = m->g.res, *gci = m->g.ci;
	int i;
	double **A = m->q.A;		/* A matrix of interpoint weights */
	int acols  = m->q.acols;	/* A matrix columns needed */
	int *xcol  = m->q.xcol;		/* A array column translation from packed to sparse index */ 
	double *b  = m->q.b;		/* b vector for RHS of simultabeous equation */
	double *x  = m->q.x;		/* x vector for result */

	/*
	 * The regular spline fitting problem to be solved here strongly
	 * resembles those involved in solving partial differential equation
	 * problems. The scattered data points equate to boundary conditions,
	 * while the smoothness criteria equate to partial differential equations.
	 */

	/*
	 * There are many approaches that can be used to solve the
	 * symetric positive-definite system Ax = b, where A is a
	 * sparse diagonal matrix with fringes. A direct method
	 * would be Cholesky decomposition, and this works well for
	 * the 1D case (no fringes), but for more than 1D, it generates
	 * fill-ins between the fringes. Given that the widest spaced
	 * fringes are at 2 * gres ^ (dim-1) spacing, this leads
	 * to an unacceptable storage requirement for A, at the resolutions
	 * and dimensions needed in color correction.
	 *
	 * The approaches that minimise A storage are itterative schemes,
	 * such as Gauss-Seidel relaxation, or conjugate-gradient methods.
	 * 
     * There are two methods allowed for below, depending on the
	 * value of JITTERS.
     * If JITTERS is non-zero, then there will be JITTERS passes of
	 * a combination of multi-grid, Gauss-Seidel relaxation,
	 * and conjugate gradient.
	 *
	 * The outermost loop will use a series of grid resolutions that
	 * approach the final resolution. Each solution gives us a close
	 * starting point for the next higher resolution. 
	 *
	 * The middle loop, uses Gauss-Seidel relaxation to approach
	 * the desired solution at a given grid resolution.
	 *
	 * The inner loop can use the conjugate-gradient method to solve
	 * a line of values simultaniously in a particular dimension. 
  	 * All the lines in each dimension are processed in red/black order
  	 * to optimise convergence rate.
  	 *
  	 * (conjugate gradient seems to be slower than pure relaxation, so
	 * it is not currently used.)
	 *
  	 * If JITTERS is zero, then a pure Gauss-Seidel relaxation approach
  	 * is used, with the solution elements being updated in RED-BLACK
	 * order. Experimentation seems to prove that this is the overall
  	 * fastest approach.
  	 * 
  	 * The equation Ax = b solves the fitting for the derivative of 
  	 * the fit error == 0. The error metric used is the norm(b - A * x)/norm(b).
  	 * I'm not sure if that is the best metric for the problem at hand though.
  	 * b[] is only non-zero where there are scattered data points (or a weak 
  	 * default function), so the error metric is being normalised to number
	 * of scattered data points. Perhaps normb should always be == 1.0 ?
	 * 
	 * The norm(b - A * x) is effectively the RMS error of the derivative
	 * fit, so it balances average error and peak error, but another
	 * approach might be to work on peak error, and apply Gauss-Seidel relaxation
	 * to grid points in peak error order (ie. relax the top 10% of grid
	 * points each itteration round) ??
	 *
	 */

	/* Note that we process the A[][] sparse columns in compact form */

#ifdef DEBUG_PROGRESS
	printf("Target tol = %f\n",tol);
#endif
	/* If the number of point is small, or it is just one */
	/* dimensional, solve it more directly. */
	if (m->g.bres <= 4) {	/* Don't want to multigrid below this */
		/* Solve using just conjugate-gradient */
		cj_line(ta, A, x, b, gno, acols, xcol, 0, gno, 1, 10 * gno, tol);
#ifdef DEBUG_PROGRESS
		printf("Solved at res %d using conjugate-gradient\n",gres[0]);
#endif
	} else {	/* Try relax till done */
		double lerr = 1.0, err = tol * 10.0, derr, ovsh = 1.0;
		int jitters = JITTERS;

		/* Compute an initial error */
		err = soln_err(A, x, b, m->q.normb, gno, acols, xcol);
#ifdef DEBUG_PROGRESS
		printf("Initial error res %d is %f\n",gres[0],err);
#endif

		for (i = 0; i < 500; i++) {
			if (i < jitters) {	/* conjugate-gradient and relaxation */
				lerr = err;
				err = one_itter1(ta, A, x, b, m->q.normb, gno, acols, xcol, di, gres, gci, (int)m->g.mres, tol * CONJ_TOL);
			
				derr = err/lerr;
				if (derr > 0.8)			/* We're not improving using itter1() fast enough */
					jitters = i-1;		/* Move to just relaxation */
#ifdef DEBUG_PROGRESS
				printf("one_itter1 at res %d has err %f, derr %f\n",gres[0],err,derr);
#endif
			} else {	/* Use just relaxation */
				int j, ni = 0;		/* Number of itters */
				if (i == jitters) {	/* Never done a relaxation itter before */
					ni = 1;		/* Just do one, to get estimate */
				} else {
					ni = (int)(((log(tol) - log(err)) * (double)ni)/(log(err) - log(lerr)));
					if (ni < 1)
						ni = 1;			/* Minimum of 1 at a time */
					else if (ni > MAXNI)
						ni = MAXNI;		/* Maximum of MAXNI at a time */
				}
				for (j = 0; j < ni; j++)	/* Do them in groups for efficiency */
					one_itter2(A, x, b, gno, acols, xcol, di, gres, gci, ovsh);
				lerr = err;
				err = soln_err(A, x, b, m->q.normb, gno, acols, xcol);
				derr = pow(err/lerr, 1.0/ni);
#ifdef DEBUG_PROGRESS
				printf("%d * one_itter2 at res %d has err %f, derr %f\n",ni,gres[0],err,derr);
#endif
				if (s->verbose) {
					printf("*"); fflush(stdout);
				}
			}
#ifdef OVERRLX
			if (derr > 0.7 && derr < 1.0) {
				ovsh = 1.0 * derr/0.7;
			}
#endif /* OVERRLX */
			if (err < tol || (derr <= 1.0 && derr > TOL_IMP))	/* within tol or < tol_improvement */
				break;
		}
	}
}

/* - - - - - - - - - - - - - - - - - - - - - - - -*/
/* Do one relaxation itteration of applying       */
/* cj_line to solve each line of x[] values, in   */
/* each line of each dimension. Return the        */
/* current solution error.                        */
static double
one_itter1(
	cj_arrays *ta,	/* cj_line temporary arrays */
	double **A,		/* Sparse A[][] matrix */
	double *x,		/* x[] matrix */
	double *b,		/* b[] matrix */
	double normb,	/* Norm of b[] */
	int gno,		/* Total number of unknowns */
	int acols,		/* Use colums in A[][] */
	int *xcol,		/* sparse expansion lookup array */
	int di,			/* number of dimensions */
	int *gres,		/* Grid resolution */
	int *gci,		/* Array increment for each dimension */
	int max_it,		/* maximum number of itterations to use (min gres) */
	double tol		/* Tollerance to solve line */
) {
	int e,d;
	
	/* For each dimension */
	for (d = 0; d < di; d++) {
		int ld = d == 0 ? 1 : 0;	/* lowest dim */
		int sof, gc[MXRI];

//printf("~1 doing one_itter1 for dim %d\n",d);
		for (e = 0; e < di; e++)
			gc[e] = 0;	/* init coords */
	
		/* Until we've done all lines in direction d, */
		/* processed in red/black order */
		for (sof = 0, e = 0; e < di;) {

			/* Solve a line */
//printf("~~solve line start %d, inc %d, len %d\n",sof,gci[d],gres[d]);
			cj_line(ta, A, x, b, gno, acols, xcol, sof, gres[d], gci[d], max_it, tol);

			/* Increment index */
			for (e = 0; e < di; e++) {
				if (e == d) 		/* Don't go in direction d */
					continue;
				if (e == ld) {
					gc[e] += 2;	/* Inc coordinate */
					sof += 2 * gci[e];	/* Track start point */
				} else {
					gc[e] += 1;	/* Inc coordinate */
					sof += 1 * gci[e];	/* Track start point */
				}
				if (gc[e] < gres[e])
					break;	/* No carry */
				gc[e] -= gres[e];			/* Reset coord */
				sof -= gres[e] * gci[e];	/* Track start point */

				if ((gres[e] & 1) == 0) {	/* Compensate for odd grid */
				    if ((gc[ld] & 1) == 1) {
						gc[ld] -= 1;		/* XOR lsb */
						sof -= gci[ld];
					} else {
						gc[ld] += 1;
						sof += gci[ld];
					}
				}
			}
			/* Stop on reaching 0 */
			for(e = 0; e < di; e++)
				if (gc[e] != 0)
					break;
		}
	}

	return soln_err(A, x, b, normb, gno, acols, xcol);
}

/* - - - - - - - - - - - - - - - - - - - - - - - -*/
/* Do one relaxation itteration of applying       */
/* direct relaxation to x[] values, in   */
/* red/black order */
static void
one_itter2(
	double **A,		/* Sparse A[][] matrix */
	double *x,		/* x[] matrix */
	double *b,		/* b[] matrix */
	int gno,		/* Total number of unknowns */
	int acols,		/* Use colums in A[][] */
	int *xcol,		/* sparse expansion lookup array */
	int di,			/* number of dimensions */
	int *gres,		/* Grid resolution */
	int *gci,		/* Array increment for each dimension */
	double ovsh		/* Overshoot to use, 1.0 for none */
) {
	int e,i,k;
	int gc[MXRI];

	for (i = e = 0; e < di; e++)
		gc[e] = 0;	/* init coords */

	for (e = 0; e < di;) {
		int k0,k1,k2,k3;
		double sm = 0.0;

		/* Right of diagonal in 4's */
		for (k = 1, k3 = i+xcol[k+3]; (k+3) < acols && k3 < gno; k += 4, k3 = i+xcol[k+3]) {
			k0 = i + xcol[k+0];
			k1 = i + xcol[k+1];
			k2 = i + xcol[k+2];
			sm += A[i][k+0] * x[k0];
			sm += A[i][k+1] * x[k1];
			sm += A[i][k+2] * x[k2];
			sm += A[i][k+3] * x[k3];
		}
		/* Finish any remaining */
		for (k3 = i + xcol[k]; k < acols && k3 < gno; k++, k3 = i + xcol[k])
			sm += A[i][k] * x[k3];

		/* Left of diagonal in 4's */
		/* (We take advantage of the symetry: what would be in the row */
		/*  to the left is repeated in the column above.) */
		for (k = 1, k3 = i-xcol[k+3]; (k+3) < acols && k3 >= 0; k += 4, k3 = i-xcol[k+3]) {
			k0 = i-xcol[k+0];
			k1 = i-xcol[k+1];
			k2 = i-xcol[k+2];
			sm += A[k0][k+0] * x[k0];
			sm += A[k1][k+1] * x[k1];
			sm += A[k2][k+2] * x[k2];
			sm += A[k3][k+3] * x[k3];
		}
		/* Finish any remaining */
		for (k3 = i-xcol[k]; k < acols && k3 >= 0; k++, k3 = i-xcol[k])
			sm += A[k3][k] * x[k3];

//		x[i] = (b[i] - sm)/A[i][0];
		x[i] += ovsh * ((b[i] - sm)/A[i][0] - x[i]);

#ifdef RED_BLACK
		/* Increment index */
		for (e = 0; e < di; e++) {
			if (e == 0) {
				gc[0] += 2;	/* Inc coordinate by 2 */
				i += 2;		/* Track start point */
			} else {
				gc[e] += 1;		/* Inc coordinate */
				i += gci[e];	/* Track start point */
			}
			if (gc[e] < gres[e])
				break;	/* No carry */
			gc[e] -= gres[e];				/* Reset coord */
			i -= gres[e] * gci[e];			/* Track start point */

			if ((gres[e] & 1) == 0) {		/* Compensate for odd grid */
				gc[0] ^= 1; 			/* XOR lsb */
				i ^= 1;
			}
		}
		/* Stop on reaching 0 */
		for(e = 0; e < di; e++)
			if (gc[e] != 0)
				break;
#else
		if (++i >= gno)
			break;
#endif
	}
}

/* - - - - - - - - - - - - - - - - - - - - - - - -*/
/* This function returns the current solution error. */
static double
soln_err(
	double **A,		/* Sparse A[][] matrix */
	double *x,		/* x[] matrix */
	double *b,		/* b[] matrix */
	double normb,	/* Norm of b[] */
	int gno,		/* Total number of unknowns */
	int acols,		/* Use colums in A[][] */
	int *xcol		/* sparse expansion lookup array */
) {
	int i, k;
	double resid;

	/* Compute norm of b - A * x */
	resid = 0.0;
	for (i = 0; i < gno; i++) {
		int k0,k1,k2,k3;
		double sm = 0.0;

		/* Diagonal and to right in 4's */
		for (k = 0, k3 = i+xcol[k+3]; (k+3) < acols && k3 < gno; k += 4, k3 = i+xcol[k+3]) {
			k0 = i + xcol[k+0];
			k1 = i + xcol[k+1];
			k2 = i + xcol[k+2];
			sm += A[i][k+0] * x[k0];
			sm += A[i][k+1] * x[k1];
			sm += A[i][k+2] * x[k2];
			sm += A[i][k+3] * x[k3];
		}
		/* Finish any remaining */
		for (k3 = i + xcol[k]; k < acols && k3 < gno; k++, k3 = i + xcol[k])
			sm += A[i][k] * x[k3];

		/* Left of diagonal in 4's */
		/* (We take advantage of the symetry: what would be in the row */
		/*  to the left is repeated in the column above.) */
		for (k = 1, k3 = i-xcol[k+3]; (k+3) < acols && k3 >= 0; k += 4, k3 = i-xcol[k+3]) {
			k0 = i-xcol[k+0];
			k1 = i-xcol[k+1];
			k2 = i-xcol[k+2];
			sm += A[k0][k+0] * x[k0];
			sm += A[k1][k+1] * x[k1];
			sm += A[k2][k+2] * x[k2];
			sm += A[k3][k+3] * x[k3];
		}
		/* Finish any remaining */
		for (k3 = i-xcol[k]; k < acols && k3 >= 0; k++, k3 = i-xcol[k])
			sm += A[k3][k] * x[k3];

		sm = b[i] - sm;
		resid += sm * sm;
	}
	resid = sqrt(resid);

	return resid/normb;
}

/* - - - - - - - - - - - - - - - - - - - - - - - -*/

/* Init temporary vectors */
static void init_cj_arrays(cj_arrays *ta) {
	memset((void *)ta, 0, sizeof(cj_arrays));
}

/* Alloc, or re-alloc temporary vectors */
static void realloc_cj_arrays(cj_arrays *ta, int nid) {

	if (nid > ta->l_nid) {
		if (ta->l_nid > 0) {
			free_dvector(ta->z,0,ta->l_nid);
			free_dvector(ta->r,0,ta->l_nid);
			free_dvector(ta->q,0,ta->l_nid);
			free_dvector(ta->xx,0,ta->l_nid);
			free_dvector(ta->n,0,ta->l_nid);
		}
		if ((ta->n = dvector(0,nid)) == NULL)
			error("Malloc of n[] failed");
		if ((ta->z = dvector(0,nid)) == NULL)
			error("Malloc of z[] failed");
		if ((ta->xx = dvector(0,nid)) == NULL)
			error("Malloc of xx[] failed");
		if ((ta->q = dvector(0,nid)) == NULL)
			error("Malloc of q[] failed");
		if ((ta->r = dvector(0,nid)) == NULL)
			error("Malloc of r[] failed");
		ta->l_nid = nid;
	}
}

/* De-alloc temporary vectors */
static void free_cj_arrays(cj_arrays *ta) {

	if (ta->l_nid > 0) {
		free_dvector(ta->z,0,ta->l_nid);
		free_dvector(ta->r,0,ta->l_nid);
		free_dvector(ta->q,0,ta->l_nid);
		free_dvector(ta->xx,0,ta->l_nid);
		free_dvector(ta->n,0,ta->l_nid);
	}
}


/* This function applies the conjugate gradient   */
/* algorithm to completely solve a line of values */
/* in one of the dimensions of the grid.          */
/* Return the normalised tollerance achieved.     */
/* This is used by an outer relaxation algorithm  */
static double
cj_line(
	cj_arrays *ta,	/* Temporary array data */
	double **A,		/* Sparse A[][] matrix */
	double *x,		/* x[] matrix */
	double *b,		/* b[] matrix */
	int gno,		/* Total number of unknowns */
	int acols,		/* Use colums in A[][] */
	int *xcol,		/* sparse expansion lookup array */
	int sof,		/* start offset of x[] to be found */
	int nid,		/* Number in dimension */
	int inc,		/* Increment to move in lines dimension */
	int max_it,		/* maximum number of itterations to use (min nid) */
	double tol		/* Normalised tollerance to stop on */
) {
	int i, ii, k, it;
	double sm;
	double resid;
	double alpha, rho = 0.0, rho_1 = 0.0;
	double normb;
	int eof = sof + nid * inc;	/* End offset */

	/* Alloc, or re-alloc temporary vectors */
	realloc_cj_arrays(ta, nid);

	/* Compute initial norm of b[] */
	for (sm = 0.0, ii = sof; ii < eof; ii += inc)
		sm += b[ii] * b[ii];
	normb = sqrt(sm);
	if (normb == 0.0) 
		normb = 1.0;

	/* Compute r = b - A * x */
	for (i = 0, ii = sof; i < nid; i++, ii += inc) {
		int k0,k1,k2,k3;
		sm = 0.0;

		/* Diagonal and to right in 4's */
		for (k = 0, k3 = ii+xcol[k+3]; (k+3) < acols && k3 < gno; k += 4, k3 = ii+xcol[k+3]) {
			k0 = ii + xcol[k+0];
			k1 = ii + xcol[k+1];
			k2 = ii + xcol[k+2];
			sm += A[ii][k+0] * x[k0];
			sm += A[ii][k+1] * x[k1];
			sm += A[ii][k+2] * x[k2];
			sm += A[ii][k+3] * x[k3];
		}
		/* Finish any remaining */
		for (k3 = ii + xcol[k]; k < acols && k3 < gno; k++, k3 = ii + xcol[k])
			sm += A[ii][k] * x[k3];

		/* Left of diagonal in 4's */
		/* (We take advantage of the symetry: what would be in the row */
		/*  to the left is repeated in the column above.) */
		for (k = 1, k3 = ii-xcol[k+3]; (k+3) < acols && k3 >= 0; k += 4, k3 = ii-xcol[k+3]) {
			k0 = ii-xcol[k+0];
			k1 = ii-xcol[k+1];
			k2 = ii-xcol[k+2];
			sm += A[k0][k+0] * x[k0];
			sm += A[k1][k+1] * x[k1];
			sm += A[k2][k+2] * x[k2];
			sm += A[k3][k+3] * x[k3];
		}
		/* Finish any remaining */
		for (k3 = ii-xcol[k]; k < acols && k3 >= 0; k++, k3 = ii-xcol[k])
			sm += A[k3][k] * x[k3];

		ta->r[i] = b[ii] - sm;
	}

	/* Transfer the x[] values we are trying to solve into */
	/* temporary xx[]. The values of interest in x[] will be */
	/* used to hold the p[] values, so that q = A * p can be */
	/* computed in the context of the x[] values we are not */
	/* trying to solve. */
	/* We also zero out p[] (== x[] in range), to compute n[]. */
	/* n[] is used to normalize the q = A * p calculation. If we */
	/* were solving all x[], then q = A * p will be 0 for p = 0. */
	/* Since we are only solving some x[], this will not be true. */
	/* We compensate for this by computing q = A * p - n */
	/* (Note that n[] could probably be combined with b[]) */

	for (i = 0, ii = sof; i < nid; i++, ii += inc) {
		ta->xx[i] = x[ii];
		x[ii] = 0.0;
	}
	/* Compute n = A * 0 */
	for (i = 0, ii = sof; i < nid; i++, ii += inc) {
		sm = 0.0;
		for (k = 0; k < acols && (ii+xcol[k]) < gno; k++)
			sm += A[ii][k] * x[ii+xcol[k]];			/* Diagonal and to right */
		for (k = 1; k < acols && (ii-xcol[k]) >= 0; k++)
			sm += A[ii-xcol[k]][k] * x[ii-xcol[k]];	/* Left of diagonal */
		ta->n[i] = sm;
	}

	/* Compute initial error = norm of r[] */
	for (sm = 0.0, i = 0; i < nid; i++)
		sm += ta->r[i] * ta->r[i];
	resid = sqrt(sm)/normb;

	/* Initial conditions don't need improvement */
	if (resid <= tol) {
		tol = resid;
		max_it = 0;
	}

	for (it = 1; it <= max_it; it++) {

		/* Aproximately solve for z[] given r[], */
		/* and also compute rho = r.z */
		for (rho = 0.0, i = 0, ii = sof; i < nid; i++, ii += inc) {
			sm = A[ii][0];
			ta->z[i] = sm != 0.0 ? ta->r[i] / sm : ta->r[i]; 	/* Simple aprox soln. */
			rho += ta->r[i] * ta->z[i];
		}

		if (it == 1) {
			for (i = 0, ii = sof; i < nid; i++, ii += inc)
				x[ii] = ta->z[i];
		} else {
			sm = rho / rho_1;
			for (i = 0, ii = sof; i < nid; i++, ii += inc)
				x[ii] = ta->z[i] + sm * x[ii];
		}
		/* Compute q = A * p  - n, */
		/* and also alpha = p.q */
		for (alpha = 0.0, i = 0, ii = sof; i < nid; i++, ii += inc) {
			sm = A[ii][0] * x[ii];
			for (k = 1; k < acols; k++) {
				int pxk = xcol[k];
				int nxk = ii-pxk;
				pxk += ii;
				if (pxk < gno)
					sm += A[ii][k] * x[pxk];
				if (nxk >= 0)
					sm += A[nxk][k] * x[nxk];
			}
			ta->q[i] = sm - ta->n[i];
			alpha += ta->q[i] * x[ii];
		}

		if (alpha != 0.0)
			alpha = rho / alpha;
		else
			alpha = 0.5;	/* ?????? */
		    
		/* Adjust soln and residual vectors, */
		/* and also norm of r[] */
		for (resid = 0.0, i = 0, ii = sof; i < nid; i++, ii += inc) {
			ta->xx[i] += alpha * x[ii];
			ta->r[i]  -= alpha * ta->q[i];
			resid += ta->r[i] * ta->r[i];
		}
		resid = sqrt(resid)/normb;

		/* If we're done as far as we want */
		if (resid <= tol) {
			tol = resid;
			max_it = it;
			break;
		}
		rho_1 = rho;
	}
	/* Substitute solution xx[] back into x[] */
	for (i = 0, ii = sof; i < nid; i++, ii += inc)
		x[ii] = ta->xx[i];

//	printf("~~ CJ Itters = %d, tol = %f\n",max_it,tol);
	return tol;
}

/* ============================================ */















