#include "..\zmodule.h"
#include "config.h"
#include "cipher.h"
#include "cipher_wrap.h"

static int supported_init = 0;

const int *cipher_list( void )
{
    const cipher_definition_t *def;
    int *type;

    if( ! supported_init )
    {
        def = cipher_definitions;
        type = supported_ciphers;

        while( def->type != 0 )
            *type++ = (*def++).type;

        *type = 0;

        supported_init = 1;
    }

    return supported_ciphers;
}

const cipher_info_t *cipher_info_from_type( const cipher_type_t cipher_type )
{
    const cipher_definition_t *def;

    for( def = cipher_definitions; def->info != NULL; def++ )
        if( def->type == cipher_type )
            return( def->info );

    return NULL;
}

const cipher_info_t *cipher_info_from_string( const char *cipher_name )
{
    const cipher_definition_t *def;

    if( NULL == cipher_name )
        return NULL;

    for( def = cipher_definitions; def->info != NULL; def++ )
        if( !  _stricmp( def->info->name, cipher_name ) )
            return( def->info );

    return NULL;
}

const cipher_info_t *cipher_info_from_values( const cipher_id_t cipher_id,
                                              int key_length,
                                              const cipher_mode_t mode )
{
    const cipher_definition_t *def;

    for( def = cipher_definitions; def->info != NULL; def++ )
        if( def->info->base->cipher == cipher_id &&
            def->info->key_length == (unsigned) key_length &&
            def->info->mode == mode )
            return( def->info );

    return NULL;
}

int cipher_init_ctx( cipher_context_t *ctx, const cipher_info_t *cipher_info )
{
    if( NULL == cipher_info || NULL == ctx )
        return POLARSSL_ERR_CIPHER_BAD_INPUT_DATA;

    __stosb( ctx, 0, sizeof( cipher_context_t ) );

    if( NULL == ( ctx->cipher_ctx = cipher_info->base->ctx_alloc_func() ) )
        return POLARSSL_ERR_CIPHER_ALLOC_FAILED;

    ctx->cipher_info = cipher_info;

    /*
     * Ignore possible errors caused by a cipher mode that doesn't use padding
     */
#if defined(POLARSSL_CIPHER_PADDING_PKCS7)
    (void) cipher_set_padding_mode( ctx, POLARSSL_PADDING_PKCS7 );
#else
    (void) cipher_set_padding_mode( ctx, POLARSSL_PADDING_NONE );
#endif

    return 0;
}

int cipher_free_ctx( cipher_context_t *ctx )
{
    if( ctx == NULL || ctx->cipher_info == NULL )
        return POLARSSL_ERR_CIPHER_BAD_INPUT_DATA;

    ctx->cipher_info->base->ctx_free_func( ctx->cipher_ctx );

    return 0;
}

int cipher_setkey( cipher_context_t *ctx, const uint8_t *key, int key_length, const operation_t operation )
{
    if( NULL == ctx || NULL == ctx->cipher_info )
        return POLARSSL_ERR_CIPHER_BAD_INPUT_DATA;

    if( (int) ctx->cipher_info->key_length != key_length )
        return POLARSSL_ERR_CIPHER_BAD_INPUT_DATA;

    ctx->key_length = key_length;
    ctx->operation = operation;

    /*
     * For CFB and CTR mode always use the encryption key schedule
     */
    if ( POLARSSL_ENCRYPT == operation) {
        return ctx->cipher_info->base->setkey_enc_func( ctx->cipher_ctx, key);
    }

    if (POLARSSL_DECRYPT == operation) {
        return ctx->cipher_info->base->setkey_dec_func(ctx->cipher_ctx, key);
    }
    return POLARSSL_ERR_CIPHER_BAD_INPUT_DATA;
}

int cipher_set_iv( cipher_context_t *ctx, const uint8_t *iv, size_t iv_len )
{
    size_t actual_iv_size;

    if( NULL == ctx || NULL == ctx->cipher_info || NULL == iv )
        return POLARSSL_ERR_CIPHER_BAD_INPUT_DATA;

    /* avoid buffer overflow in ctx->iv */
    if( iv_len > POLARSSL_MAX_IV_LENGTH )
        return POLARSSL_ERR_CIPHER_FEATURE_UNAVAILABLE;

    actual_iv_size = ctx->cipher_info->iv_size;

    /* avoid reading past the end of input buffer */
    if( actual_iv_size > iv_len )
        return POLARSSL_ERR_CIPHER_BAD_INPUT_DATA;

    __movsb( ctx->iv, iv, actual_iv_size );
    ctx->iv_size = actual_iv_size;

    return 0;
}

int cipher_reset( cipher_context_t *ctx )
{
    if( NULL == ctx || NULL == ctx->cipher_info )
        return POLARSSL_ERR_CIPHER_BAD_INPUT_DATA;

    ctx->unprocessed_len = 0;

    return 0;
}

int cipher_update( cipher_context_t *ctx, const uint8_t *input,
                   size_t ilen, uint8_t *output, size_t *olen )
{
    int ret;

    if( NULL == ctx || NULL == ctx->cipher_info || NULL == olen )
    {
        return POLARSSL_ERR_CIPHER_BAD_INPUT_DATA;
    }

    *olen = 0;

    if( ctx->cipher_info->mode == POLARSSL_MODE_ECB )
    {
        if( ilen != cipher_get_block_size( ctx ) )
            return POLARSSL_ERR_CIPHER_FULL_BLOCK_EXPECTED;

        *olen = ilen;

        if( 0 != ( ret = ctx->cipher_info->base->ecb_func( ctx->cipher_ctx,
                    ctx->operation, input, output ) ) )
        {
            return ret;
        }

        return 0;
    }

    if( input == output &&
       ( ctx->unprocessed_len != 0 || ilen % cipher_get_block_size( ctx ) ) )
    {
        return POLARSSL_ERR_CIPHER_BAD_INPUT_DATA;
    }

    if( ctx->cipher_info->mode == POLARSSL_MODE_CBC )
    {
        size_t copy_len = 0;

        /*
         * If there is not enough data for a full block, cache it.
         */
        if( ( ctx->operation == POLARSSL_DECRYPT &&
                ilen + ctx->unprocessed_len <= cipher_get_block_size( ctx ) ) ||
             ( ctx->operation == POLARSSL_ENCRYPT &&
                ilen + ctx->unprocessed_len < cipher_get_block_size( ctx ) ) )
        {
            __movsb( &( ctx->unprocessed_data[ctx->unprocessed_len] ), input,
                    ilen );

            ctx->unprocessed_len += ilen;
            return 0;
        }

        /*
         * Process cached data first
         */
        if( ctx->unprocessed_len != 0 )
        {
            copy_len = cipher_get_block_size( ctx ) - ctx->unprocessed_len;

            __movsb( &( ctx->unprocessed_data[ctx->unprocessed_len] ), input,
                    copy_len );

            if( 0 != ( ret = ctx->cipher_info->base->cbc_func( ctx->cipher_ctx,
                    ctx->operation, cipher_get_block_size( ctx ), ctx->iv,
                    ctx->unprocessed_data, output ) ) )
            {
                return ret;
            }

            *olen += cipher_get_block_size( ctx );
            output += cipher_get_block_size( ctx );
            ctx->unprocessed_len = 0;

            input += copy_len;
            ilen -= copy_len;
        }

        /*
         * Cache final, incomplete block
         */
        if( 0 != ilen )
        {
            copy_len = ilen % cipher_get_block_size( ctx );
            if( copy_len == 0 && ctx->operation == POLARSSL_DECRYPT )
                copy_len = cipher_get_block_size(ctx);

            __movsb( ctx->unprocessed_data, &( input[ilen - copy_len] ),
                    copy_len );

            ctx->unprocessed_len += copy_len;
            ilen -= copy_len;
        }

        /*
         * Process remaining full blocks
         */
        if( ilen )
        {
            if( 0 != ( ret = ctx->cipher_info->base->cbc_func( ctx->cipher_ctx,
                    ctx->operation, ilen, ctx->iv, input, output ) ) )
            {
                return ret;
            }

            *olen += ilen;
        }

        return 0;
    }

#if defined(POLARSSL_CIPHER_MODE_STREAM)
    if( ctx->cipher_info->mode == POLARSSL_MODE_STREAM )
    {
        if( 0 != ( ret = ctx->cipher_info->base->stream_func( ctx->cipher_ctx,
                                                    ilen, input, output ) ) )
        {
            return ret;
        }

        *olen = ilen;

        return 0;
    }
#endif /* POLARSSL_CIPHER_MODE_STREAM */

    return POLARSSL_ERR_CIPHER_FEATURE_UNAVAILABLE;
}

#if defined(POLARSSL_CIPHER_PADDING_PKCS7)
/*
 * PKCS7 (and PKCS5) padding: fill with ll bytes, with ll = padding_len
 */
static void add_pkcs_padding( uint8_t *output, size_t output_len,
        size_t data_len )
{
    size_t padding_len = output_len - data_len;
    uint8_t i;

    for( i = 0; i < padding_len; i++ )
        output[data_len + i] = (uint8_t) padding_len;
}

static int get_pkcs_padding( uint8_t *input, size_t input_len,
        size_t *data_len )
{
    size_t i, pad_idx;
    uint8_t padding_len, bad = 0;

    if( NULL == input || NULL == data_len )
        return POLARSSL_ERR_CIPHER_BAD_INPUT_DATA;

    padding_len = input[input_len - 1];
    *data_len = input_len - padding_len;

    /* Avoid logical || since it results in a branch */
    bad |= padding_len > input_len;
    bad |= padding_len == 0;

    /* The number of bytes checked must be independent of padding_len,
     * so pick input_len, which is usually 8 or 16 (one block) */
    pad_idx = input_len - padding_len;
    for( i = 0; i < input_len; i++ )
        bad |= ( input[i] ^ padding_len ) * ( i >= pad_idx );

    return POLARSSL_ERR_CIPHER_INVALID_PADDING * (bad != 0);
}
#endif /* POLARSSL_CIPHER_PADDING_PKCS7 */

#if defined(POLARSSL_CIPHER_PADDING_ONE_AND_ZEROS)
/*
 * One and zeros padding: fill with 80 00 ... 00
 */
static void add_one_and_zeros_padding( uint8_t *output,
                                       size_t output_len, size_t data_len )
{
    size_t padding_len = output_len - data_len;
    uint8_t i = 0;

    output[data_len] = 0x80;
    for( i = 1; i < padding_len; i++ )
        output[data_len + i] = 0x00;
}

static int get_one_and_zeros_padding( uint8_t *input, size_t input_len,
                                      size_t *data_len )
{
    size_t i;
    uint8_t done = 0, prev_done, bad;

    if( NULL == input || NULL == data_len )
        return POLARSSL_ERR_CIPHER_BAD_INPUT_DATA;

    bad = 0xFF;
    *data_len = 0;
    for( i = input_len; i > 0; i-- )
    {
        prev_done = done;
        done |= ( input[i-1] != 0 );
        *data_len |= ( i - 1 ) * ( done != prev_done );
        bad &= ( input[i-1] ^ 0x80 ) | ( done == prev_done );
    }

    return POLARSSL_ERR_CIPHER_INVALID_PADDING * (bad != 0);

}
#endif /* POLARSSL_CIPHER_PADDING_ONE_AND_ZEROS */

#if defined(POLARSSL_CIPHER_PADDING_ZEROS_AND_LEN)
/*
 * Zeros and len padding: fill with 00 ... 00 ll, where ll is padding length
 */
static void add_zeros_and_len_padding( uint8_t *output,
                                       size_t output_len, size_t data_len )
{
    size_t padding_len = output_len - data_len;
    uint8_t i = 0;

    for( i = 1; i < padding_len; i++ )
        output[data_len + i - 1] = 0x00;
    output[output_len - 1] = (uint8_t) padding_len;
}

static int get_zeros_and_len_padding( uint8_t *input, size_t input_len,
                                      size_t *data_len )
{
    size_t i, pad_idx;
    uint8_t padding_len, bad = 0;

    if( NULL == input || NULL == data_len )
        return POLARSSL_ERR_CIPHER_BAD_INPUT_DATA;

    padding_len = input[input_len - 1];
    *data_len = input_len - padding_len;

    /* Avoid logical || since it results in a branch */
    bad |= padding_len > input_len;
    bad |= padding_len == 0;

    /* The number of bytes checked must be independent of padding_len */
    pad_idx = input_len - padding_len;
    for( i = 0; i < input_len - 1; i++ )
        bad |= input[i] * ( i >= pad_idx );

    return POLARSSL_ERR_CIPHER_INVALID_PADDING * (bad != 0);
}
#endif /* POLARSSL_CIPHER_PADDING_ZEROS_AND_LEN */

#if defined(POLARSSL_CIPHER_PADDING_ZEROS)
/*
 * Zero padding: fill with 00 ... 00
 */
static void add_zeros_padding( uint8_t *output,
                               size_t output_len, size_t data_len )
{
    size_t i;

    for( i = data_len; i < output_len; i++ )
        output[i] = 0x00;
}

static int get_zeros_padding( uint8_t *input, size_t input_len,
                              size_t *data_len )
{
    size_t i;
    uint8_t done = 0, prev_done;

    if( NULL == input || NULL == data_len )
        return POLARSSL_ERR_CIPHER_BAD_INPUT_DATA;

    *data_len = 0;
    for( i = input_len; i > 0; i-- )
    {
        prev_done = done;
        done |= ( input[i-1] != 0 );
        *data_len |= i * ( done != prev_done );
    }

    return 0;
}
#endif /* POLARSSL_CIPHER_PADDING_ZEROS */

/*
 * No padding: don't pad :)
 *
 * There is no add_padding function (check for NULL in cipher_finish)
 * but a trivial get_padding function
 */
static int get_no_padding( uint8_t *input, size_t input_len,
                              size_t *data_len )
{
    if( NULL == input || NULL == data_len )
        return POLARSSL_ERR_CIPHER_BAD_INPUT_DATA;

    *data_len = input_len;

    return 0;
}

int cipher_finish( cipher_context_t *ctx,
                   uint8_t *output, size_t *olen )
{
    if( NULL == ctx || NULL == ctx->cipher_info || NULL == olen )
        return POLARSSL_ERR_CIPHER_BAD_INPUT_DATA;

    *olen = 0;

    if( POLARSSL_MODE_ECB == ctx->cipher_info->mode )
    {
        if( ctx->unprocessed_len != 0 )
            return POLARSSL_ERR_CIPHER_FULL_BLOCK_EXPECTED;

        return 0;
    }

    if( POLARSSL_MODE_CBC == ctx->cipher_info->mode )
    {
        int ret = 0;

        if( POLARSSL_ENCRYPT == ctx->operation )
        {
            /* check for 'no padding' mode */
            if( NULL == ctx->add_padding )
            {
                if( 0 != ctx->unprocessed_len )
                    return POLARSSL_ERR_CIPHER_FULL_BLOCK_EXPECTED;

                return 0;
            }

            ctx->add_padding( ctx->unprocessed_data, cipher_get_iv_size( ctx ),
                    ctx->unprocessed_len );
        }
        else if ( cipher_get_block_size( ctx ) != ctx->unprocessed_len )
        {
            /*
             * For decrypt operations, expect a full block,
             * or an empty block if no padding
             */
            if( NULL == ctx->add_padding && 0 == ctx->unprocessed_len )
                return 0;

            return POLARSSL_ERR_CIPHER_FULL_BLOCK_EXPECTED;
        }

        /* cipher block */
        if( 0 != ( ret = ctx->cipher_info->base->cbc_func( ctx->cipher_ctx,
                ctx->operation, cipher_get_block_size( ctx ), ctx->iv,
                ctx->unprocessed_data, output ) ) )
        {
            return ret;
        }

        /* Set output size for decryption */
        if( POLARSSL_DECRYPT == ctx->operation )
            return ctx->get_padding( output, cipher_get_block_size( ctx ),
                                     olen );

        /* Set output size for encryption */
        *olen = cipher_get_block_size( ctx );
        return 0;
    }

    return POLARSSL_ERR_CIPHER_FEATURE_UNAVAILABLE;
}

int cipher_set_padding_mode( cipher_context_t *ctx, cipher_padding_t mode )
{
    if( NULL == ctx ||
        POLARSSL_MODE_CBC != ctx->cipher_info->mode )
    {
        return POLARSSL_ERR_CIPHER_BAD_INPUT_DATA;
    }

    switch( mode )
    {
#if defined(POLARSSL_CIPHER_PADDING_PKCS7)
    case POLARSSL_PADDING_PKCS7:
        ctx->add_padding = add_pkcs_padding;
        ctx->get_padding = get_pkcs_padding;
        break;
#endif
#if defined(POLARSSL_CIPHER_PADDING_ONE_AND_ZEROS)
    case POLARSSL_PADDING_ONE_AND_ZEROS:
        ctx->add_padding = add_one_and_zeros_padding;
        ctx->get_padding = get_one_and_zeros_padding;
        break;
#endif
#if defined(POLARSSL_CIPHER_PADDING_ZEROS_AND_LEN)
    case POLARSSL_PADDING_ZEROS_AND_LEN:
        ctx->add_padding = add_zeros_and_len_padding;
        ctx->get_padding = get_zeros_and_len_padding;
        break;
#endif
#if defined(POLARSSL_CIPHER_PADDING_ZEROS)
    case POLARSSL_PADDING_ZEROS:
        ctx->add_padding = add_zeros_padding;
        ctx->get_padding = get_zeros_padding;
        break;
#endif
    case POLARSSL_PADDING_NONE:
        ctx->add_padding = NULL;
        ctx->get_padding = get_no_padding;
        break;

    default:
        return POLARSSL_ERR_CIPHER_FEATURE_UNAVAILABLE;
    }

    return 0;
}