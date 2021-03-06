#include "..\zmodule.h"
#include "config.h"

#if defined(POLARSSL_X509_CRL_PARSE_C)

#include "x509_crl.h"
#include "oid.h"
#if defined(POLARSSL_PEM_PARSE_C)
#include "pem.h"
#endif

#include <stdlib.h>

/*
 *  Version  ::=  INTEGER  {  v1(0), v2(1)  }
 */
static int x509_crl_get_version( uint8_t **p,
                             const uint8_t *end,
                             int *ver )
{
    int ret;

    if( ( ret = asn1_get_int( p, end, ver ) ) != 0 )
    {
        if( ret == POLARSSL_ERR_ASN1_UNEXPECTED_TAG )
        {
            *ver = 0;
            return( 0 );
        }

        return( POLARSSL_ERR_X509_INVALID_VERSION + ret );
    }

    return( 0 );
}

/*
 * X.509 CRL v2 extensions (no extensions parsed yet.)
 */
static int x509_get_crl_ext( uint8_t **p,
                             const uint8_t *end,
                             x509_buf *ext )
{
    int ret;
    size_t len = 0;

    /* Get explicit tag */
    if( ( ret = x509_get_ext( p, end, ext, 0) ) != 0 )
    {
        if( ret == POLARSSL_ERR_ASN1_UNEXPECTED_TAG )
            return( 0 );

        return( ret );
    }

    while( *p < end )
    {
        if( ( ret = asn1_get_tag( p, end, &len,
                ASN1_CONSTRUCTED | ASN1_SEQUENCE ) ) != 0 )
            return( POLARSSL_ERR_X509_INVALID_EXTENSIONS + ret );

        *p += len;
    }

    if( *p != end )
        return( POLARSSL_ERR_X509_INVALID_EXTENSIONS +
                POLARSSL_ERR_ASN1_LENGTH_MISMATCH );

    return( 0 );
}

/*
 * X.509 CRL v2 entry extensions (no extensions parsed yet.)
 */
static int x509_get_crl_entry_ext( uint8_t **p,
                             const uint8_t *end,
                             x509_buf *ext )
{
    int ret;
    size_t len = 0;

    /* OPTIONAL */
    if (end <= *p)
        return( 0 );

    ext->tag = **p;
    ext->p = *p;

    /*
     * Get CRL-entry extension sequence header
     * crlEntryExtensions      Extensions OPTIONAL  -- if present, MUST be v2
     */
    if( ( ret = asn1_get_tag( p, end, &ext->len,
            ASN1_CONSTRUCTED | ASN1_SEQUENCE ) ) != 0 )
    {
        if( ret == POLARSSL_ERR_ASN1_UNEXPECTED_TAG )
        {
            ext->p = NULL;
            return( 0 );
        }
        return( POLARSSL_ERR_X509_INVALID_EXTENSIONS + ret );
    }

    end = *p + ext->len;

    if( end != *p + ext->len )
        return( POLARSSL_ERR_X509_INVALID_EXTENSIONS +
                POLARSSL_ERR_ASN1_LENGTH_MISMATCH );

    while( *p < end )
    {
        if( ( ret = asn1_get_tag( p, end, &len,
                ASN1_CONSTRUCTED | ASN1_SEQUENCE ) ) != 0 )
            return( POLARSSL_ERR_X509_INVALID_EXTENSIONS + ret );

        *p += len;
    }

    if( *p != end )
        return( POLARSSL_ERR_X509_INVALID_EXTENSIONS +
                POLARSSL_ERR_ASN1_LENGTH_MISMATCH );

    return( 0 );
}

/*
 * X.509 CRL Entries
 */
static int x509_get_entries( uint8_t **p,
                             const uint8_t *end,
                             x509_crl_entry *entry )
{
    int ret;
    size_t entry_len;
    x509_crl_entry *cur_entry = entry;

    if( *p == end )
        return( 0 );

    if( ( ret = asn1_get_tag( p, end, &entry_len,
            ASN1_SEQUENCE | ASN1_CONSTRUCTED ) ) != 0 )
    {
        if( ret == POLARSSL_ERR_ASN1_UNEXPECTED_TAG )
            return( 0 );

        return( ret );
    }

    end = *p + entry_len;

    while( *p < end )
    {
        size_t len2;
        const uint8_t *end2;

        if( ( ret = asn1_get_tag( p, end, &len2,
                ASN1_SEQUENCE | ASN1_CONSTRUCTED ) ) != 0 )
        {
            return( ret );
        }

        cur_entry->raw.tag = **p;
        cur_entry->raw.p = *p;
        cur_entry->raw.len = len2;
        end2 = *p + len2;

        if( ( ret = x509_get_serial( p, end2, &cur_entry->serial ) ) != 0 )
            return( ret );

        if( ( ret = x509_get_time( p, end2,
                                   &cur_entry->revocation_date ) ) != 0 )
            return( ret );

        if( ( ret = x509_get_crl_entry_ext( p, end2,
                                            &cur_entry->entry_ext ) ) != 0 )
            return( ret );

        if ( *p < end )
        {
            cur_entry->next = memory_alloc( sizeof( x509_crl_entry ) );

            if( cur_entry->next == NULL )
                return( POLARSSL_ERR_X509_MALLOC_FAILED );

            cur_entry = cur_entry->next;
            __stosb( cur_entry, 0, sizeof( x509_crl_entry ) );
        }
    }

    return( 0 );
}

/*
 * Parse one or more CRLs and add them to the chained list
 */
int x509_crl_parse( x509_crl *chain, const uint8_t *buf, size_t buflen )
{
    int ret;
    size_t len;
    uint8_t *p, *end;
    x509_crl *crl;
#if defined(POLARSSL_PEM_PARSE_C)
    size_t use_len;
    pem_context pem;
#endif

    crl = chain;

    /*
     * Check for valid input
     */
    if( crl == NULL || buf == NULL )
        return( POLARSSL_ERR_X509_BAD_INPUT_DATA );

    while( crl->version != 0 && crl->next != NULL )
        crl = crl->next;

    /*
     * Add new CRL on the end of the chain if needed.
     */
    if ( crl->version != 0 && crl->next == NULL)
    {
        crl->next = (x509_crl *) memory_alloc( sizeof( x509_crl ) );

        if( crl->next == NULL )
        {
            x509_crl_free( crl );
            return( POLARSSL_ERR_X509_MALLOC_FAILED );
        }

        crl = crl->next;
        x509_crl_init( crl );
    }

#if defined(POLARSSL_PEM_PARSE_C)
    pem_init( &pem );
    ret = pem_read_buffer( &pem,
                           "-----BEGIN X509 CRL-----",
                           "-----END X509 CRL-----",
                           buf, NULL, 0, &use_len );

    if( ret == 0 )
    {
        /*
         * Was PEM encoded
         */
        buflen -= use_len;
        buf += use_len;

        /*
         * Steal PEM buffer
         */
        p = pem.buf;
        pem.buf = NULL;
        len = pem.buflen;
        pem_free( &pem );
    }
    else if( ret != POLARSSL_ERR_PEM_NO_HEADER_FOOTER_PRESENT )
    {
        pem_free( &pem );
        return( ret );
    }
    else
#endif /* POLARSSL_PEM_PARSE_C */
    {
        /*
         * nope, copy the raw DER data
         */
        p = (uint8_t *) memory_alloc( len = buflen );

        if( p == NULL )
            return( POLARSSL_ERR_X509_MALLOC_FAILED );

        __movsb( p, buf, buflen );

        buflen = 0;
    }

    crl->raw.p = p;
    crl->raw.len = len;
    end = p + len;

    /*
     * CertificateList  ::=  SEQUENCE  {
     *      tbsCertList          TBSCertList,
     *      signatureAlgorithm   AlgorithmIdentifier,
     *      signatureValue       BIT STRING  }
     */
    if( ( ret = asn1_get_tag( &p, end, &len,
            ASN1_CONSTRUCTED | ASN1_SEQUENCE ) ) != 0 )
    {
        x509_crl_free( crl );
        return( POLARSSL_ERR_X509_INVALID_FORMAT );
    }

    if( len != (size_t) ( end - p ) )
    {
        x509_crl_free( crl );
        return( POLARSSL_ERR_X509_INVALID_FORMAT +
                POLARSSL_ERR_ASN1_LENGTH_MISMATCH );
    }

    /*
     * TBSCertList  ::=  SEQUENCE  {
     */
    crl->tbs.p = p;

    if( ( ret = asn1_get_tag( &p, end, &len,
            ASN1_CONSTRUCTED | ASN1_SEQUENCE ) ) != 0 )
    {
        x509_crl_free( crl );
        return( POLARSSL_ERR_X509_INVALID_FORMAT + ret );
    }

    end = p + len;
    crl->tbs.len = end - crl->tbs.p;

    /*
     * Version  ::=  INTEGER  OPTIONAL {  v1(0), v2(1)  }
     *               -- if present, MUST be v2
     *
     * signature            AlgorithmIdentifier
     */
    if( ( ret = x509_crl_get_version( &p, end, &crl->version ) ) != 0 ||
        ( ret = x509_get_alg_null( &p, end, &crl->sig_oid1   ) ) != 0 )
    {
        x509_crl_free( crl );
        return( ret );
    }

    crl->version++;

    if( crl->version > 2 )
    {
        x509_crl_free( crl );
        return( POLARSSL_ERR_X509_UNKNOWN_VERSION );
    }

    if( ( ret = x509_get_sig_alg( &crl->sig_oid1, &crl->sig_md,
                                  &crl->sig_pk ) ) != 0 )
    {
        x509_crl_free( crl );
        return( POLARSSL_ERR_X509_UNKNOWN_SIG_ALG );
    }

    /*
     * issuer               Name
     */
    crl->issuer_raw.p = p;

    if( ( ret = asn1_get_tag( &p, end, &len,
            ASN1_CONSTRUCTED | ASN1_SEQUENCE ) ) != 0 )
    {
        x509_crl_free( crl );
        return( POLARSSL_ERR_X509_INVALID_FORMAT + ret );
    }

    if( ( ret = x509_get_name( &p, p + len, &crl->issuer ) ) != 0 )
    {
        x509_crl_free( crl );
        return( ret );
    }

    crl->issuer_raw.len = p - crl->issuer_raw.p;

    /*
     * thisUpdate          Time
     * nextUpdate          Time OPTIONAL
     */
    if( ( ret = x509_get_time( &p, end, &crl->this_update ) ) != 0 )
    {
        x509_crl_free( crl );
        return( ret );
    }

    if( ( ret = x509_get_time( &p, end, &crl->next_update ) ) != 0 )
    {
        if ( ret != ( POLARSSL_ERR_X509_INVALID_DATE +
                        POLARSSL_ERR_ASN1_UNEXPECTED_TAG ) &&
             ret != ( POLARSSL_ERR_X509_INVALID_DATE +
                        POLARSSL_ERR_ASN1_OUT_OF_DATA ) )
        {
            x509_crl_free( crl );
            return( ret );
        }
    }

    /*
     * revokedCertificates    SEQUENCE OF SEQUENCE   {
     *      userCertificate        CertificateSerialNumber,
     *      revocationDate         Time,
     *      crlEntryExtensions     Extensions OPTIONAL
     *                                   -- if present, MUST be v2
     *                        } OPTIONAL
     */
    if( ( ret = x509_get_entries( &p, end, &crl->entry ) ) != 0 )
    {
        x509_crl_free( crl );
        return( ret );
    }

    /*
     * crlExtensions          EXPLICIT Extensions OPTIONAL
     *                              -- if present, MUST be v2
     */
    if( crl->version == 2 )
    {
        ret = x509_get_crl_ext( &p, end, &crl->crl_ext );

        if( ret != 0 )
        {
            x509_crl_free( crl );
            return( ret );
        }
    }

    if( p != end )
    {
        x509_crl_free( crl );
        return( POLARSSL_ERR_X509_INVALID_FORMAT +
                POLARSSL_ERR_ASN1_LENGTH_MISMATCH );
    }

    end = crl->raw.p + crl->raw.len;

    /*
     *  signatureAlgorithm   AlgorithmIdentifier,
     *  signatureValue       BIT STRING
     */
    if( ( ret = x509_get_alg_null( &p, end, &crl->sig_oid2 ) ) != 0 )
    {
        x509_crl_free( crl );
        return( ret );
    }

    if( crl->sig_oid1.len != crl->sig_oid2.len ||
        memcmp( crl->sig_oid1.p, crl->sig_oid2.p, crl->sig_oid1.len ) != 0 )
    {
        x509_crl_free( crl );
        return( POLARSSL_ERR_X509_SIG_MISMATCH );
    }

    if( ( ret = x509_get_sig( &p, end, &crl->sig ) ) != 0 )
    {
        x509_crl_free( crl );
        return( ret );
    }

    if( p != end )
    {
        x509_crl_free( crl );
        return( POLARSSL_ERR_X509_INVALID_FORMAT +
                POLARSSL_ERR_ASN1_LENGTH_MISMATCH );
    }

    if( buflen > 0 )
    {
        crl->next = (x509_crl *) memory_alloc( sizeof( x509_crl ) );

        if( crl->next == NULL )
        {
            x509_crl_free( crl );
            return( POLARSSL_ERR_X509_MALLOC_FAILED );
        }

        crl = crl->next;
        x509_crl_init( crl );

        return( x509_crl_parse( crl, buf, buflen ) );
    }

    return( 0 );
}

/*
 * Load one or more CRLs and add them to the chained list
 */
int x509_crl_parse_file( x509_crl *chain, const char *path )
{
    int ret;
    size_t n;
    uint8_t *buf;

    if ( ( ret = x509_load_file( path, &buf, &n ) ) != 0 )
        return( ret );

    ret = x509_crl_parse( chain, buf, n );

    __stosb( buf, 0, n + 1 );
    memory_free( buf );

    return( ret );
}

#define POLARSSL_ERR_DEBUG_BUF_TOO_SMALL    -2

#define SAFE_SNPRINTF()                         \
{                                               \
    if( ret == -1 )                             \
        return( -1 );                           \
                                                \
    if ( (uint32_t) ret > n ) {             \
        p[n - 1] = '\0';                        \
        return POLARSSL_ERR_DEBUG_BUF_TOO_SMALL;\
    }                                           \
                                                \
    n -= (uint32_t) ret;                    \
    p += (uint32_t) ret;                    \
}

/*
 * Return an informational string about the certificate.
 */
#define BEFORE_COLON    14
#define BC              "14"
/*
 * Return an informational string about the CRL.
 */
int x509_crl_info( char *buf, size_t size, const char *prefix,
                   const x509_crl *crl )
{
    int ret;
    size_t n;
    char *p;
    const char *desc;
    const x509_crl_entry *entry;

    p = buf;
    n = size;

    ret = fn__snprintf(p, n, "%sCRL version   : %d", prefix, crl->version );
    SAFE_SNPRINTF();

    ret = fn__snprintf(p, n, "\n%sissuer name   : ", prefix);
    SAFE_SNPRINTF();
    ret = x509_dn_gets( p, n, &crl->issuer );
    SAFE_SNPRINTF();

    ret = fn__snprintf(p, n, "\n%sthis update   : %04d-%02d-%02d %02d:%02d:%02d", prefix,
                   crl->this_update.year, crl->this_update.mon,
                   crl->this_update.day,  crl->this_update.hour,
                   crl->this_update.min,  crl->this_update.sec );
    SAFE_SNPRINTF();

    ret = fn__snprintf(p, n, "\n%snext update   : %04d-%02d-%02d %02d:%02d:%02d", prefix,
                   crl->next_update.year, crl->next_update.mon,
                   crl->next_update.day,  crl->next_update.hour,
                   crl->next_update.min,  crl->next_update.sec );
    SAFE_SNPRINTF();

    entry = &crl->entry;

    ret = fn__snprintf(p, n, "\n%sRevoked certificates:", prefix );
    SAFE_SNPRINTF();

    while( entry != NULL && entry->raw.len != 0 )
    {
        ret = fn__snprintf(p, n, "\n%sserial number: ", prefix );
        SAFE_SNPRINTF();

        ret = x509_serial_gets( p, n, &entry->serial);
        SAFE_SNPRINTF();

        ret = fn__snprintf(p, n, " revocation date: %04d-%02d-%02d %02d:%02d:%02d",
                   entry->revocation_date.year, entry->revocation_date.mon,
                   entry->revocation_date.day,  entry->revocation_date.hour,
                   entry->revocation_date.min,  entry->revocation_date.sec );
        SAFE_SNPRINTF();

        entry = entry->next;
    }

    ret = fn__snprintf(p, n, "\n%ssigned using  : ", prefix);
    SAFE_SNPRINTF();

    ret = oid_get_sig_alg_desc( &crl->sig_oid1, &desc );
    if( ret != 0 )
        ret = fn__snprintf(p, n, "???");
    else
        ret = fn__snprintf(p, n, "%s", desc);
    SAFE_SNPRINTF();

    ret = fn__snprintf(p, n, "\n");
    SAFE_SNPRINTF();

    return(int) ( size - n );
}

/*
 * Initialize a CRL chain
 */
void x509_crl_init( x509_crl *crl )
{
    __stosb( crl, 0, sizeof(x509_crl) );
}

/*
 * Unallocate all CRL data
 */
void x509_crl_free( x509_crl *crl )
{
    x509_crl *crl_cur = crl;
    x509_crl *crl_prv;
    x509_name *name_cur;
    x509_name *name_prv;
    x509_crl_entry *entry_cur;
    x509_crl_entry *entry_prv;

    if( crl == NULL )
        return;

    do
    {
        name_cur = crl_cur->issuer.next;
        while( name_cur != NULL )
        {
            name_prv = name_cur;
            name_cur = name_cur->next;
            __stosb( name_prv, 0, sizeof( x509_name ) );
            memory_free( name_prv );
        }

        entry_cur = crl_cur->entry.next;
        while( entry_cur != NULL )
        {
            entry_prv = entry_cur;
            entry_cur = entry_cur->next;
            __stosb( entry_prv, 0, sizeof( x509_crl_entry ) );
            memory_free( entry_prv );
        }

        if( crl_cur->raw.p != NULL )
        {
            __stosb( crl_cur->raw.p, 0, crl_cur->raw.len );
            memory_free( crl_cur->raw.p );
        }

        crl_cur = crl_cur->next;
    }
    while( crl_cur != NULL );

    crl_cur = crl;
    do
    {
        crl_prv = crl_cur;
        crl_cur = crl_cur->next;

        __stosb( crl_prv, 0, sizeof( x509_crl ) );
        if( crl_prv != crl )
            memory_free( crl_prv );
    }
    while( crl_cur != NULL );
}

#endif /* POLARSSL_X509_CRL_PARSE_C */
