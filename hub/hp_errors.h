#ifndef HP_ERRORS_H
#define HP_ERRORS_H

/* Positive error codes; functions return the negated value. */
#define HP_E_INVAL  1
#define HP_E_TOOBIG 2
#define HP_E_BUSY   3
#define HP_E_IO     4
#define HP_E_NOENT  5
#define HP_E_NOSYS  6 /* not implemented by this backend */
#define HP_E_HALBUSY 7 /* /dev/i2s is held (stock hal still running): quiesce first */

#endif
