#include "trgm.h"

#include "access/gist.h"
#include "access/itup.h"
#include "access/rtree.h"
#include "utils/elog.h"
#include "utils/palloc.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "storage/bufpage.h"
#include "access/tuptoaster.h"

PG_FUNCTION_INFO_V1(gtrgm_in);
Datum		gtrgm_in(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(gtrgm_out);
Datum		gtrgm_out(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(gtrgm_compress);
Datum		gtrgm_compress(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(gtrgm_decompress);
Datum		gtrgm_decompress(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(gtrgm_consistent);
Datum		gtrgm_consistent(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(gtrgm_union);
Datum		gtrgm_union(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(gtrgm_same);
Datum		gtrgm_same(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(gtrgm_penalty);
Datum		gtrgm_penalty(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(gtrgm_picksplit);
Datum		gtrgm_picksplit(PG_FUNCTION_ARGS);

#define GETENTRY(vec,pos) ((TRGM *) DatumGetPointer((vec)->vector[(pos)].key))

#define SUMBIT(val) (		 \
	GETBITBYTE(val,0) + \
	GETBITBYTE(val,1) + \
	GETBITBYTE(val,2) + \
	GETBITBYTE(val,3) + \
	GETBITBYTE(val,4) + \
	GETBITBYTE(val,5) + \
	GETBITBYTE(val,6) + \
	GETBITBYTE(val,7)	\
)


Datum
gtrgm_in(PG_FUNCTION_ARGS)
{
	elog(ERROR, "Not implemented");
	PG_RETURN_DATUM(0);
}

Datum
gtrgm_out(PG_FUNCTION_ARGS)
{
	elog(ERROR, "Not implemented");
	PG_RETURN_DATUM(0);
}

static void
makesign(BITVECP sign, TRGM * a)
{
	int4		k,
				len = ARRNELEM(a);
	trgm	   *ptr = GETARR(a);
	int4		tmp = 0;

	MemSet((void *) sign, 0, sizeof(BITVEC));
	SETBIT(sign, SIGLENBIT);	/* set last unused bit */
	for (k = 0; k < len; k++)
	{
		CPTRGM(((char *) &tmp), ptr + k);
		HASH(sign, tmp);
	}
}

Datum
gtrgm_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval = entry;

	if (entry->leafkey)
	{							/* trgm */
		TRGM	   *res;
		text	   *val = (text *) DatumGetPointer(PG_DETOAST_DATUM(entry->key));

		res = generate_trgm(VARDATA(val), VARSIZE(val) - VARHDRSZ);
		retval = (GISTENTRY *) palloc(sizeof(GISTENTRY));
		gistentryinit(*retval, PointerGetDatum(res),
					  entry->rel, entry->page,
					  entry->offset, res->len, FALSE);
	}
	else if (ISSIGNKEY(DatumGetPointer(entry->key)) &&
			 !ISALLTRUE(DatumGetPointer(entry->key)))
	{
		int4		i,
					len;
		TRGM	   *res;
		BITVECP		sign = GETSIGN(DatumGetPointer(entry->key));

		LOOPBYTE(
				 if ((sign[i] & 0xff) != 0xff)
				 PG_RETURN_POINTER(retval);
		);

		len = CALCGTSIZE(SIGNKEY | ALLISTRUE, 0);
		res = (TRGM *) palloc(len);
		res->len = len;
		res->flag = SIGNKEY | ALLISTRUE;

		retval = (GISTENTRY *) palloc(sizeof(GISTENTRY));
		gistentryinit(*retval, PointerGetDatum(res),
					  entry->rel, entry->page,
					  entry->offset, res->len, FALSE);
	}
	PG_RETURN_POINTER(retval);
}

Datum
gtrgm_decompress(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(PG_GETARG_DATUM(0));
}

Datum
gtrgm_consistent(PG_FUNCTION_ARGS)
{
	text	   *query = (text *) PG_GETARG_TEXT_P(1);
	TRGM	   *key = (TRGM *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(0))->key);
	TRGM	   *qtrg = generate_trgm(VARDATA(query), VARSIZE(query) - VARHDRSZ);
	int			res = false;

	if (GIST_LEAF((GISTENTRY *) PG_GETARG_POINTER(0)))
	{							/* all leafs contains orig trgm */
		float4		tmpsml = cnt_sml(key, qtrg);

		/* strange bug at freebsd 5.2.1 and gcc 3.3.3 */
		res = (*(int *) &tmpsml == *(int *) &trgm_limit || tmpsml > trgm_limit) ? true : false;
	}
	else if (ISALLTRUE(key))
	{							/* non-leaf contains signature */
		res = true;
	}
	else
	{							/* non-leaf contains signature */
		int4		count = 0;
		int4		k,
					len = ARRNELEM(qtrg);
		trgm	   *ptr = GETARR(qtrg);
		BITVECP		sign = GETSIGN(key);
		int4		tmp = 0;

		for (k = 0; k < len; k++)
		{
			CPTRGM(((char *) &tmp), ptr + k);
			count += GETBIT(sign, HASHVAL(tmp));
		}
#ifdef DIVUNION
		res = (len == count) ? true : ((((((float4) count) / ((float4) (len - count)))) >= trgm_limit) ? true : false);
#else
		res = (len == 0) ? false : ((((((float4) count) / ((float4) len))) >= trgm_limit) ? true : false);
#endif
	}

	PG_RETURN_BOOL(res);
}

static int4
unionkey(BITVECP sbase, TRGM * add)
{
	int4		i;

	if (ISSIGNKEY(add))
	{
		BITVECP		sadd = GETSIGN(add);

		if (ISALLTRUE(add))
			return 1;

		LOOPBYTE(
				 sbase[i] |= sadd[i];
		);
	}
	else
	{
		trgm	   *ptr = GETARR(add);
		int4		tmp = 0;

		for (i = 0; i < ARRNELEM(add); i++)
		{
			CPTRGM(((char *) &tmp), ptr + i);
			HASH(sbase, tmp);
		}
	}
	return 0;
}


Datum
gtrgm_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	int4		len = entryvec->n;
	int		   *size = (int *) PG_GETARG_POINTER(1);
	BITVEC		base;
	int4		i;
	int4		flag = 0;
	TRGM	   *result;

	MemSet((void *) base, 0, sizeof(BITVEC));
	for (i = 0; i < len; i++)
	{
		if (unionkey(base, GETENTRY(entryvec, i)))
		{
			flag = ALLISTRUE;
			break;
		}
	}

	flag |= SIGNKEY;
	len = CALCGTSIZE(flag, 0);
	result = (TRGM *) palloc(len);
	*size = result->len = len;
	result->flag = flag;
	if (!ISALLTRUE(result))
		memcpy((void *) GETSIGN(result), (void *) base, sizeof(BITVEC));

	PG_RETURN_POINTER(result);
}

Datum
gtrgm_same(PG_FUNCTION_ARGS)
{
	TRGM	   *a = (TRGM *) PG_GETARG_POINTER(0);
	TRGM	   *b = (TRGM *) PG_GETARG_POINTER(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	if (ISSIGNKEY(a))
	{							/* then b also ISSIGNKEY */
		if (ISALLTRUE(a) && ISALLTRUE(b))
			*result = true;
		else if (ISALLTRUE(a))
			*result = false;
		else if (ISALLTRUE(b))
			*result = false;
		else
		{
			int4		i;
			BITVECP		sa = GETSIGN(a),
						sb = GETSIGN(b);

			*result = true;
			LOOPBYTE(
					 if (sa[i] != sb[i])
					 {
				*result = false;
				break;
			}
			);
		}
	}
	else
	{							/* a and b ISARRKEY */
		int4		lena = ARRNELEM(a),
					lenb = ARRNELEM(b);

		if (lena != lenb)
			*result = false;
		else
		{
			trgm	   *ptra = GETARR(a),
					   *ptrb = GETARR(b);
			int4		i;

			*result = true;
			for (i = 0; i < lena; i++)
				if (CMPTRGM(ptra + i, ptrb + i))
				{
					*result = false;
					break;
				}
		}
	}

	PG_RETURN_POINTER(result);
}

static int4
sizebitvec(BITVECP sign)
{
	int4		size = 0,
				i;

	LOOPBYTE(
			 size += SUMBIT(*(char *) sign);
	sign = (BITVECP) (((char *) sign) + 1);
	);
	return size;
}

static int
hemdistsign(BITVECP a, BITVECP b)
{
	int			i,
				dist = 0;

	LOOPBIT(
			if (GETBIT(a, i) != GETBIT(b, i))
			dist++;
	);
	return dist;
}

static int
hemdist(TRGM * a, TRGM * b)
{
	if (ISALLTRUE(a))
	{
		if (ISALLTRUE(b))
			return 0;
		else
			return SIGLENBIT - sizebitvec(GETSIGN(b));
	}
	else if (ISALLTRUE(b))
		return SIGLENBIT - sizebitvec(GETSIGN(a));

	return hemdistsign(GETSIGN(a), GETSIGN(b));
}

Datum
gtrgm_penalty(PG_FUNCTION_ARGS)
{
	GISTENTRY  *origentry = (GISTENTRY *) PG_GETARG_POINTER(0); /* always ISSIGNKEY */
	GISTENTRY  *newentry = (GISTENTRY *) PG_GETARG_POINTER(1);
	float	   *penalty = (float *) PG_GETARG_POINTER(2);
	TRGM	   *origval = (TRGM *) DatumGetPointer(origentry->key);
	TRGM	   *newval = (TRGM *) DatumGetPointer(newentry->key);
	BITVECP		orig = GETSIGN(origval);

	*penalty = 0.0;

	if (ISARRKEY(newval))
	{
		BITVEC		sign;

		makesign(sign, newval);

		if (ISALLTRUE(origval))
			*penalty = ((float) (SIGLENBIT - sizebitvec(sign))) / (float) (SIGLENBIT + 1);
		else
			*penalty = hemdistsign(sign, orig);
	}
	else
		*penalty = hemdist(origval, newval);
	PG_RETURN_POINTER(penalty);
}

typedef struct
{
	bool		allistrue;
	BITVEC		sign;
}	CACHESIGN;

static void
fillcache(CACHESIGN * item, TRGM * key)
{
	item->allistrue = false;
	if (ISARRKEY(key))
		makesign(item->sign, key);
	else if (ISALLTRUE(key))
		item->allistrue = true;
	else
		memcpy((void *) item->sign, (void *) GETSIGN(key), sizeof(BITVEC));
}

#define WISH_F(a,b,c) (double)( -(double)(((a)-(b))*((a)-(b))*((a)-(b)))*(c) )
typedef struct
{
	OffsetNumber pos;
	int4		cost;
} SPLITCOST;

static int
comparecost(const void *a, const void *b)
{
	if (((SPLITCOST *) a)->cost == ((SPLITCOST *) b)->cost)
		return 0;
	else
		return (((SPLITCOST *) a)->cost > ((SPLITCOST *) b)->cost) ? 1 : -1;
}


static int
hemdistcache(CACHESIGN * a, CACHESIGN * b)
{
	if (a->allistrue)
	{
		if (b->allistrue)
			return 0;
		else
			return SIGLENBIT - sizebitvec(b->sign);
	}
	else if (b->allistrue)
		return SIGLENBIT - sizebitvec(a->sign);

	return hemdistsign(a->sign, b->sign);
}

Datum
gtrgm_picksplit(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	OffsetNumber maxoff = entryvec->n - 2;
	GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);
	OffsetNumber k,
				j;
	TRGM	   *datum_l,
			   *datum_r;
	BITVECP		union_l,
				union_r;
	int4		size_alpha,
				size_beta;
	int4		size_waste,
				waste = -1;
	int4		nbytes;
	OffsetNumber seed_1 = 0,
				seed_2 = 0;
	OffsetNumber *left,
			   *right;
	BITVECP		ptr;
	int			i;
	CACHESIGN  *cache;
	SPLITCOST  *costvector;

	nbytes = (maxoff + 2) * sizeof(OffsetNumber);
	v->spl_left = (OffsetNumber *) palloc(nbytes);
	v->spl_right = (OffsetNumber *) palloc(nbytes);

	cache = (CACHESIGN *) palloc(sizeof(CACHESIGN) * (maxoff + 2));
	fillcache(&cache[FirstOffsetNumber], GETENTRY(entryvec, FirstOffsetNumber));

	for (k = FirstOffsetNumber; k < maxoff; k = OffsetNumberNext(k))
	{
		for (j = OffsetNumberNext(k); j <= maxoff; j = OffsetNumberNext(j))
		{
			if (k == FirstOffsetNumber)
				fillcache(&cache[j], GETENTRY(entryvec, j));

			size_waste = hemdistcache(&(cache[j]), &(cache[k]));
			if (size_waste > waste)
			{
				waste = size_waste;
				seed_1 = k;
				seed_2 = j;
			}
		}
	}

	left = v->spl_left;
	v->spl_nleft = 0;
	right = v->spl_right;
	v->spl_nright = 0;

	if (seed_1 == 0 || seed_2 == 0)
	{
		seed_1 = 1;
		seed_2 = 2;
	}

	/* form initial .. */
	if (cache[seed_1].allistrue)
	{
		datum_l = (TRGM *) palloc(CALCGTSIZE(SIGNKEY | ALLISTRUE, 0));
		datum_l->len = CALCGTSIZE(SIGNKEY | ALLISTRUE, 0);
		datum_l->flag = SIGNKEY | ALLISTRUE;
	}
	else
	{
		datum_l = (TRGM *) palloc(CALCGTSIZE(SIGNKEY, 0));
		datum_l->len = CALCGTSIZE(SIGNKEY, 0);
		datum_l->flag = SIGNKEY;
		memcpy((void *) GETSIGN(datum_l), (void *) cache[seed_1].sign, sizeof(BITVEC));
	}
	if (cache[seed_2].allistrue)
	{
		datum_r = (TRGM *) palloc(CALCGTSIZE(SIGNKEY | ALLISTRUE, 0));
		datum_r->len = CALCGTSIZE(SIGNKEY | ALLISTRUE, 0);
		datum_r->flag = SIGNKEY | ALLISTRUE;
	}
	else
	{
		datum_r = (TRGM *) palloc(CALCGTSIZE(SIGNKEY, 0));
		datum_r->len = CALCGTSIZE(SIGNKEY, 0);
		datum_r->flag = SIGNKEY;
		memcpy((void *) GETSIGN(datum_r), (void *) cache[seed_2].sign, sizeof(BITVEC));
	}

	union_l = GETSIGN(datum_l);
	union_r = GETSIGN(datum_r);
	maxoff = OffsetNumberNext(maxoff);
	fillcache(&cache[maxoff], GETENTRY(entryvec, maxoff));
	/* sort before ... */
	costvector = (SPLITCOST *) palloc(sizeof(SPLITCOST) * maxoff);
	for (j = FirstOffsetNumber; j <= maxoff; j = OffsetNumberNext(j))
	{
		costvector[j - 1].pos = j;
		size_alpha = hemdistcache(&(cache[seed_1]), &(cache[j]));
		size_beta = hemdistcache(&(cache[seed_2]), &(cache[j]));
		costvector[j - 1].cost = abs(size_alpha - size_beta);
	}
	qsort((void *) costvector, maxoff, sizeof(SPLITCOST), comparecost);

	for (k = 0; k < maxoff; k++)
	{
		j = costvector[k].pos;
		if (j == seed_1)
		{
			*left++ = j;
			v->spl_nleft++;
			continue;
		}
		else if (j == seed_2)
		{
			*right++ = j;
			v->spl_nright++;
			continue;
		}

		if (ISALLTRUE(datum_l) || cache[j].allistrue)
		{
			if (ISALLTRUE(datum_l) && cache[j].allistrue)
				size_alpha = 0;
			else
				size_alpha = SIGLENBIT - sizebitvec(
													(cache[j].allistrue) ? GETSIGN(datum_l) : GETSIGN(cache[j].sign)
					);
		}
		else
			size_alpha = hemdistsign(cache[j].sign, GETSIGN(datum_l));

		if (ISALLTRUE(datum_r) || cache[j].allistrue)
		{
			if (ISALLTRUE(datum_r) && cache[j].allistrue)
				size_beta = 0;
			else
				size_beta = SIGLENBIT - sizebitvec(
												   (cache[j].allistrue) ? GETSIGN(datum_r) : GETSIGN(cache[j].sign)
					);
		}
		else
			size_beta = hemdistsign(cache[j].sign, GETSIGN(datum_r));

		if (size_alpha < size_beta + WISH_F(v->spl_nleft, v->spl_nright, 0.1))
		{
			if (ISALLTRUE(datum_l) || cache[j].allistrue)
			{
				if (!ISALLTRUE(datum_l))
					MemSet((void *) GETSIGN(datum_l), 0xff, sizeof(BITVEC));
			}
			else
			{
				ptr = cache[j].sign;
				LOOPBYTE(
						 union_l[i] |= ptr[i];
				);
			}
			*left++ = j;
			v->spl_nleft++;
		}
		else
		{
			if (ISALLTRUE(datum_r) || cache[j].allistrue)
			{
				if (!ISALLTRUE(datum_r))
					MemSet((void *) GETSIGN(datum_r), 0xff, sizeof(BITVEC));
			}
			else
			{
				ptr = cache[j].sign;
				LOOPBYTE(
						 union_r[i] |= ptr[i];
				);
			}
			*right++ = j;
			v->spl_nright++;
		}
	}

	*right = *left = FirstOffsetNumber;
	v->spl_ldatum = PointerGetDatum(datum_l);
	v->spl_rdatum = PointerGetDatum(datum_r);

	PG_RETURN_POINTER(v);
}
