/*
 * Copyright (c) 2018 Fastly, Kazuho Oku
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include "picotls/openssl.h"
#include "h2o/string_.h"
#include "h2o/http3_common.h"
#include "h2o/http3_internal.h"

struct st_h2o_http3_ingress_unistream_t {
    /**
     * back pointer
     */
    quicly_stream_t *quic;
    /**
     *
     */
    h2o_buffer_t *recvbuf;
    /**
     *
     */
    int (*handle_input)(h2o_http3_conn_t *conn, struct st_h2o_http3_ingress_unistream_t *stream, const uint8_t **src,
                        const uint8_t *src_end, const char **err_desc);
};

struct st_h2o_http3_egress_unistream_t {
    /**
     * back pointer
     */
    quicly_stream_t *quic;
    /**
     *
     */
    h2o_buffer_t *sendbuf;
};

/**
 * maximum payload size excluding DATA frame; stream receive window MUST be at least as big as this
 */
#define MAX_FRAME_SIZE 16384

const ptls_iovec_t h2o_http3_alpn[1] = {{(void *)H2O_STRLIT("h3-17")}};

static void ingress_unistream_on_destroy(quicly_stream_t *qs)
{
    struct st_h2o_http3_ingress_unistream_t *stream = qs->data;
    h2o_buffer_dispose(&stream->recvbuf);
    free(stream);
}

static int ingress_unistream_on_receive(quicly_stream_t *qs, size_t off, const void *input, size_t len)
{
    h2o_http3_conn_t *conn = *quicly_get_data(qs->conn);
    struct st_h2o_http3_ingress_unistream_t *stream = qs->data;
    int ret;

    /* save received data */
    if ((ret = h2o_http3_update_recvbuf(&stream->recvbuf, off, input, len)) != 0)
        return ret;

    /* respond with fatal error if the stream is closed */
    if (quicly_recvstate_transfer_complete(&stream->quic->recvstate))
        return H2O_HTTP3_ERROR_CLOSED_CRITICAL_STREAM;

    /* determine bytes that can be handled */
    const uint8_t *src = (const uint8_t *)stream->recvbuf->bytes,
                  *src_end = src + quicly_recvstate_bytes_available(&stream->quic->recvstate);
    if (src == src_end)
        return 0;

    /* handle the bytes (TODO retain err_desc) */
    const char *err_desc = NULL;
    ret = stream->handle_input(conn, stream, &src, src_end, &err_desc);

    /* remove bytes that have been consumed */
    size_t bytes_consumed = src - (const uint8_t *)stream->recvbuf->bytes;
    if (bytes_consumed != 0) {
        h2o_buffer_consume(&stream->recvbuf, bytes_consumed);
        quicly_stream_sync_recvbuf(stream->quic, bytes_consumed);
    }

    return ret;
}

static int ingress_unistream_on_receive_reset(quicly_stream_t *qs, int err)
{
    return H2O_HTTP3_ERROR_CLOSED_CRITICAL_STREAM;
}

static int qpack_encoder_stream_handle_input(h2o_http3_conn_t *conn, struct st_h2o_http3_ingress_unistream_t *stream,
                                             const uint8_t **src, const uint8_t *src_end, const char **err_desc)
{
    while (*src != src_end) {
        int64_t *unblocked_stream_ids;
        size_t num_unblocked;
        int ret;
        if ((ret = h2o_qpack_decoder_handle_input(conn->qpack.dec, &unblocked_stream_ids, &num_unblocked, src, src_end,
                                                  err_desc)) != 0)
            return ret;
        /* TODO handle unblocked streams */
    }
    return 0;
}

static int qpack_decoder_stream_handle_input(h2o_http3_conn_t *conn, struct st_h2o_http3_ingress_unistream_t *stream,
                                             const uint8_t **src, const uint8_t *src_end, const char **err_desc)
{
    while (*src != src_end) {
        int ret;
        if ((ret = h2o_qpack_encoder_handle_input(conn->qpack.enc, src, src_end, err_desc)) != 0)
            return ret;
    }
    return 0;
}

static int control_stream_handle_input(h2o_http3_conn_t *conn, struct st_h2o_http3_ingress_unistream_t *stream, const uint8_t **src,
                                       const uint8_t *src_end, const char **err_desc)
{
    h2o_http3_read_frame_t frame;
    int ret;

    if ((ret = h2o_http3_read_frame(&frame, src, src_end, err_desc)) != 0) {
        if (ret == H2O_HTTP3_ERROR_INCOMPLETE)
            ret = 0;
        return ret;
    }

    if (h2o_http3_has_received_settings(conn) == (frame.type == H2O_HTTP3_FRAME_TYPE_SETTINGS) ||
        frame.type == H2O_HTTP3_FRAME_TYPE_DATA)
        return H2O_HTTP3_ERROR_MALFORMED_FRAME(frame.type);

    return conn->callbacks->handle_control_stream_frame(conn, frame.type, frame.payload, frame.length, err_desc);
}

static int unknown_stream_type_handle_input(h2o_http3_conn_t *conn, struct st_h2o_http3_ingress_unistream_t *stream,
                                            const uint8_t **src, const uint8_t *src_end, const char **err_desc)
{
    /* just consume the input */
    *src = src_end;
    return 0;
}

static int unknown_type_handle_input(h2o_http3_conn_t *conn, struct st_h2o_http3_ingress_unistream_t *stream, const uint8_t **src,
                                     const uint8_t *src_end, const char **err_desc)
{
    if (*src == src_end) {
        /* a sender is allowed to close or reset a unidirectional stream */
        return 0;
    }

    switch (**src) {
    case 'C':
        conn->_control_streams.ingress.control = stream;
        stream->handle_input = control_stream_handle_input;
        break;
    case 'H':
        conn->_control_streams.ingress.qpack_encoder = stream;
        stream->handle_input = qpack_encoder_stream_handle_input;
        break;
    case 'h':
        conn->_control_streams.ingress.qpack_decoder = stream;
        stream->handle_input = qpack_decoder_stream_handle_input;
        break;
    default:
        quicly_request_stop(stream->quic, H2O_HTTP3_ERROR_UNKNOWN_STREAM_TYPE);
        stream->handle_input =
            unknown_stream_type_handle_input; /* TODO reconsider quicly API; do we need to read data after sending STOP_SENDING? */
        break;
    }
    ++*src;

    return stream->handle_input(conn, stream, src, src_end, err_desc);
}

static void egress_unistream_on_destroy(quicly_stream_t *qs)
{
    struct st_h2o_http3_egress_unistream_t *stream = qs->data;
    h2o_buffer_dispose(&stream->sendbuf);
    free(stream);
}

static void egress_unistream_on_send_shift(quicly_stream_t *qs, size_t delta)
{
    struct st_h2o_http3_egress_unistream_t *stream = qs->data;
    h2o_buffer_consume(&stream->sendbuf, delta);
}

static int egress_unistream_on_send_emit(quicly_stream_t *qs, size_t off, void *dst, size_t *len, int *wrote_all)
{
    struct st_h2o_http3_egress_unistream_t *stream = qs->data;

    if (*len >= stream->sendbuf->size - off) {
        *len = stream->sendbuf->size - off;
        *wrote_all = 1;
    } else {
        *wrote_all = 0;
    }
    memcpy(dst, stream->sendbuf->bytes, *len);
    return 0;
}

static int egress_unistream_on_send_stop(quicly_stream_t *qs, int err)
{
    return H2O_HTTP3_ERROR_CLOSED_CRITICAL_STREAM;
}

void h2o_http3_on_create_unidirectional_stream(quicly_stream_t *qs)
{
    if (quicly_stream_is_self_initiated(qs)) {
        /* create egress unistream */
        static const quicly_stream_callbacks_t callbacks = {egress_unistream_on_destroy, egress_unistream_on_send_shift,
                                                            egress_unistream_on_send_emit, egress_unistream_on_send_stop};
        struct st_h2o_http3_egress_unistream_t *stream = h2o_mem_alloc(sizeof(*stream));
        qs->data = stream;
        qs->callbacks = &callbacks;
        stream->quic = qs;
        h2o_buffer_init(&stream->sendbuf, &h2o_socket_buffer_prototype);
    } else {
        /* create ingress unistream */
        static const quicly_stream_callbacks_t callbacks = {
            ingress_unistream_on_destroy, NULL, NULL, NULL, ingress_unistream_on_receive, ingress_unistream_on_receive_reset};
        struct st_h2o_http3_ingress_unistream_t *stream = h2o_mem_alloc(sizeof(*stream));
        qs->data = stream;
        qs->callbacks = &callbacks;
        stream->quic = qs;
        h2o_buffer_init(&stream->recvbuf, &h2o_socket_buffer_prototype);
        stream->handle_input = unknown_type_handle_input;
    }
}

static int open_egress_unistream(h2o_http3_conn_t *conn, struct st_h2o_http3_egress_unistream_t **stream, h2o_iovec_t initial_bytes)
{
    quicly_stream_t *qs;
    int ret;

    if ((ret = quicly_open_stream(conn->quic, &qs, 1)) != 0)
        return ret;
    *stream = qs->data;
    assert((*stream)->quic == qs);

    h2o_buffer_append(&(*stream)->sendbuf, initial_bytes.base, initial_bytes.len);
    return quicly_stream_sync_sendbuf((*stream)->quic, 1);
}

/**
 * keyed-hash to avoid collision attacks on unauthmap (FIXME switch to CMAC for better performance)
 */
static ptls_hash_context_t *accepting_hashkey_hashctx = NULL;

static void init_accepting_hashkey_hashctx(void)
{
    uint8_t random_bytes[PTLS_SHA256_DIGEST_SIZE];

    ptls_openssl_random_bytes(random_bytes, sizeof(random_bytes));
    accepting_hashkey_hashctx = ptls_hmac_create(&ptls_openssl_sha256, random_bytes, sizeof(random_bytes));
    assert(accepting_hashkey_hashctx != NULL);
}

static uint64_t calc_accepting_hashkey(struct sockaddr *sa, ptls_iovec_t offered_cid)
{
    static pthread_once_t init_once = PTHREAD_ONCE_INIT;
    uint8_t buf[1 + 16 + 4 + 1 + 18], *p = buf;
    uint64_t md[PTLS_SHA256_DIGEST_SIZE / sizeof(uint64_t)];

    *p++ = (uint8_t)sa->sa_family;
    switch (sa->sa_family) {
    case AF_INET: {
        struct sockaddr_in *sin = (void *)sa;
        H2O_BUILD_ASSERT(sizeof(sin->sin_addr) == 4);
        memcpy(p, &sin->sin_addr, 4);
        p += 4;
        p = quicly_encode16(p, sin->sin_port);
    } break;
    case AF_INET6: {
        struct sockaddr_in6 *sin6 = (void *)sa;
        H2O_BUILD_ASSERT(sizeof(sin6->sin6_addr) == 16);
        memcpy(p, &sin6->sin6_addr, 16);
        p += 16;
        p = quicly_encode16(p, sin6->sin6_port);
    } break;
    default:
        h2o_fatal("unexpected sa_family");
        break;
    }
    *p++ = (uint8_t)offered_cid.len;
    memcpy(p, offered_cid.base, offered_cid.len);
    p += offered_cid.len;
    assert(p <= buf + sizeof(buf));

    pthread_once(&init_once, init_accepting_hashkey_hashctx);

    accepting_hashkey_hashctx->update(accepting_hashkey_hashctx, buf, p - buf);
    accepting_hashkey_hashctx->final(accepting_hashkey_hashctx, md, PTLS_HASH_FINAL_MODE_RESET);

    return md[0];
}

static uint64_t calc_accepting_hashkey_from_conn(quicly_conn_t *conn)
{
    const quicly_cid_t *offered_cid = quicly_get_offered_cid(conn);
    struct sockaddr *sa;
    socklen_t salen;

    quicly_get_peername(conn, &sa, &salen);
    return calc_accepting_hashkey(sa, ptls_iovec_init(offered_cid->cid, offered_cid->len));
}

static h2o_http3_conn_t *find_connection(h2o_http3_ctx_t *ctx, struct sockaddr *sa, socklen_t salen,
                                         quicly_decoded_packet_t *packet)
{
    /* server receives Initial or 0-RTT packet (that uses client-generated CID) */
    if (packet->cid.dest.might_be_client_generated) {
        uint64_t key = calc_accepting_hashkey(sa, packet->cid.dest.encrypted);
        khiter_t iter = kh_get_h2o_http3_unauthmap(ctx->conns_accepting, key);
        if (iter != kh_end(ctx->conns_accepting)) {
            h2o_http3_conn_t *conn = kh_val(ctx->conns_accepting, iter);
            assert(!quicly_is_client(conn->quic));
            if (quicly_is_destination(conn->quic, sa, salen, packet))
                return conn;
        }
    }

    /* lookup idmap if the decrypted cid passes authentication */
    if (packet->cid.dest.plaintext.node_id == 0 && packet->cid.dest.plaintext.thread_id == 0) {
        khiter_t iter = kh_get_h2o_http3_idmap(ctx->conns_by_id, packet->cid.dest.plaintext.master_id);
        if (iter != kh_end(ctx->conns_by_id)) {
            h2o_http3_conn_t *conn = kh_val(ctx->conns_by_id, iter);
            if (quicly_is_destination(conn->quic, sa, salen, packet))
                return conn;
        }
    }

    /* FIXME create stateless reset map and recognize them */

    return NULL;
}

static void process_packets(h2o_http3_ctx_t *ctx, struct sockaddr *sa, socklen_t salen, quicly_decoded_packet_t *packets,
                            size_t num_packets)
{
    h2o_http3_conn_t *conn = find_connection(ctx, sa, salen, packets);

    if (conn != NULL) {
        size_t i;
        for (i = 0; i != num_packets; ++i) {
            /* FIXME process closure and errors */
            quicly_receive(conn->quic, packets + i);
        }
    } else if (ctx->acceptor != NULL) {
        conn = ctx->acceptor(ctx, sa, salen, packets, num_packets);
    }

    /* for locality, emit packets belonging to the same connection NOW! */
    if (conn != NULL)
        h2o_http3_send(conn);
}

static void on_read(h2o_socket_t *sock, const char *err)
{
    h2o_http3_ctx_t *ctx = sock->data;
    int fd = h2o_socket_get_fd(sock);

    while (1) {
        uint8_t buf[16384], *bufpt = buf;
        struct {
            struct msghdr mess;
            struct sockaddr_storage sa;
            struct iovec vec;
        } dgrams[32];
        size_t dgram_index, num_dgrams;
        ssize_t rret;

        /* read datagrams */
        for (dgram_index = 0; dgram_index < sizeof(dgrams) / sizeof(dgrams[0]) && buf + sizeof(buf) - bufpt > 2048; ++dgram_index) {
            /* read datagram */
            memset(&dgrams[dgram_index].mess, 0, sizeof(dgrams[dgram_index].mess));
            dgrams[dgram_index].mess.msg_name = &dgrams[dgram_index].sa;
            dgrams[dgram_index].mess.msg_namelen = sizeof(dgrams[dgram_index].sa);
            dgrams[dgram_index].vec.iov_base = bufpt;
            dgrams[dgram_index].vec.iov_len = buf + sizeof(buf) - bufpt;
            dgrams[dgram_index].mess.msg_iov = &dgrams[dgram_index].vec;
            dgrams[dgram_index].mess.msg_iovlen = 1;
            while ((rret = recvmsg(fd, &dgrams[dgram_index].mess, 0)) <= 0 && errno == EINTR)
                ;
            if (rret <= 0)
                break;
            dgrams[dgram_index].vec.iov_len = rret;
            bufpt += rret;
        }
        num_dgrams = dgram_index;
        if (num_dgrams == 0)
            break;

        /* convert dgrams to decoded packets and process */
        quicly_decoded_packet_t packets[64];
        size_t packet_index = 0;
        for (dgram_index = 0; dgram_index != num_dgrams; ++dgram_index) {
            if (packet_index != 0 && !(dgram_index == 0 || h2o_socket_compare_address(dgrams[dgram_index - 1].mess.msg_name,
                                                                                      dgrams[dgram_index].mess.msg_name))) {
                process_packets(ctx, dgrams[dgram_index - 1].mess.msg_name, dgrams[dgram_index - 1].mess.msg_namelen, packets,
                                packet_index);
                packet_index = 0;
            }
            size_t off = 0;
            while (off != dgrams[dgram_index].vec.iov_len) {
                size_t plen = quicly_decode_packet(ctx->quic, packets + packet_index, dgrams[dgram_index].vec.iov_base + off,
                                                   dgrams[dgram_index].vec.iov_len - off);
                if (plen == SIZE_MAX)
                    break;
                off += plen;
                if (packet_index == sizeof(packets) / sizeof(packets[0]) - 1 ||
                    !(packet_index == 0 ||
                      h2o_memis(packets[0].cid.dest.encrypted.base, packets[0].cid.dest.encrypted.len,
                                packets[packet_index].cid.dest.encrypted.base, packets[packet_index].cid.dest.encrypted.len))) {
                    process_packets(ctx, dgrams[dgram_index].mess.msg_name, dgrams[dgram_index].mess.msg_namelen, packets,
                                    packet_index + 1);
                    packet_index = 0;
                } else {
                    ++packet_index;
                }
            }
        }
        if (packet_index != 0)
            process_packets(ctx, dgrams[dgram_index - 1].mess.msg_name, dgrams[dgram_index - 1].mess.msg_namelen, packets,
                            packet_index);
    }
}

static void on_timeout(h2o_timer_t *timeout)
{
    h2o_http3_conn_t *conn = H2O_STRUCT_FROM_MEMBER(h2o_http3_conn_t, _timeout, timeout);
    h2o_http3_send(conn);
}

static int send_one(int fd, quicly_datagram_t *p)
{
    int ret;
    struct msghdr mess;
    struct iovec vec;
    memset(&mess, 0, sizeof(mess));
    mess.msg_name = &p->sa;
    mess.msg_namelen = p->salen;
    vec.iov_base = p->data.base;
    vec.iov_len = p->data.len;
    mess.msg_iov = &vec;
    mess.msg_iovlen = 1;
    while ((ret = (int)sendmsg(fd, &mess, 0)) == -1 && errno == EINTR)
        ;
    return ret;
}

int h2o_http3_read_frame(h2o_http3_read_frame_t *frame, const uint8_t **_src, const uint8_t *src_end, const char **err_desc)
{
    const uint8_t *src = *_src;

    if ((frame->length = quicly_decodev(&src, src_end)) == UINT64_MAX)
        return H2O_HTTP3_ERROR_INCOMPLETE;
    if (src == src_end)
        return H2O_HTTP3_ERROR_INCOMPLETE;
    frame->type = *src++;
    frame->_header_size = (uint8_t)(src - *_src);
    if (frame->type != H2O_HTTP3_FRAME_TYPE_DATA) {
        if (frame->length >= MAX_FRAME_SIZE) {
            *err_desc = "H3 frame too large";
            return H2O_HTTP3_ERROR_MALFORMED_FRAME(frame->type); /* FIXME is this the correct code? */
        }
        if (src_end - src < frame->length)
            return H2O_HTTP3_ERROR_INCOMPLETE;
        frame->payload = src;
        src += frame->length;
    }

    *_src = src;
    return 0;
}

void h2o_http3_init_context(h2o_http3_ctx_t *ctx, h2o_loop_t *loop, h2o_socket_t *sock, quicly_context_t *quic,
                            h2o_http3_accept_cb acceptor)
{
    assert(quic->stream_open != NULL);

    ctx->loop = loop;
    ctx->sock = sock;
    ctx->sock->data = ctx;
    ctx->quic = quic;
    ctx->next_cid = (quicly_cid_plaintext_t){0}; /* FIXME set thread_id, etc. */
    ctx->conns_by_id = kh_init_h2o_http3_idmap();
    ctx->conns_accepting = kh_init_h2o_http3_unauthmap();
    ctx->acceptor = acceptor;

    h2o_socket_read_start(ctx->sock, on_read);
}

void h2o_http3_dispose_context(h2o_http3_ctx_t *ctx)
{
    h2o_socket_close(ctx->sock);
    /* FIXME destroy each connection in the map? */
    kh_destroy_h2o_http3_idmap(ctx->conns_by_id);
    kh_destroy_h2o_http3_unauthmap(ctx->conns_accepting);
}

void h2o_http3_init_conn(h2o_http3_conn_t *conn, h2o_http3_ctx_t *ctx, h2o_http3_conn_callbacks_t *callbacks)
{
    *conn = (h2o_http3_conn_t){ctx, NULL, callbacks};
    h2o_timer_init(&conn->_timeout, on_timeout);
}

void h2o_http3_dispose_conn(h2o_http3_conn_t *conn)
{
    if (conn->qpack.dec != NULL)
        h2o_qpack_destroy_decoder(conn->qpack.dec);
    if (conn->qpack.enc != NULL)
        h2o_qpack_destroy_encoder(conn->qpack.enc);
    if (conn->quic != NULL) {
        khiter_t iter;
        /* unregister from maps */
        if ((iter = kh_get_h2o_http3_idmap(conn->ctx->conns_by_id, quicly_get_master_id(conn->quic)->master_id)) !=
            kh_end(conn->ctx->conns_by_id))
            kh_del_h2o_http3_idmap(conn->ctx->conns_by_id, iter);
        if (!quicly_is_client(conn->quic)) {
            uint64_t key = calc_accepting_hashkey_from_conn(conn->quic);
            if ((iter = kh_get_h2o_http3_unauthmap(conn->ctx->conns_accepting, key)) != kh_end(conn->ctx->conns_accepting))
                kh_del_h2o_http3_unauthmap(conn->ctx->conns_accepting, iter);
        }
        quicly_free(conn->quic);
    }
    h2o_timer_unlink(&conn->_timeout);
}

int h2o_http3_setup(h2o_http3_conn_t *conn, quicly_conn_t *quic)
{
    int ret;

    conn->quic = quic;
    *quicly_get_data(conn->quic) = conn;
    conn->qpack.dec = h2o_qpack_create_decoder(H2O_HTTP3_DEFAULT_HEADER_TABLE_SIZE, 100 /* FIXME */);

    { /* register to the idmap */
        int r;
        khiter_t iter = kh_put_h2o_http3_idmap(conn->ctx->conns_by_id, quicly_get_master_id(conn->quic)->master_id, &r);
        assert(iter != kh_end(conn->ctx->conns_by_id));
        kh_val(conn->ctx->conns_by_id, iter) = conn;
    }

    /* register to accepting hashmap (FIXME unregister when ... droping the Initial and 0-RTT keys?) */
    if (!quicly_is_client(conn->quic)) {
        uint64_t key = calc_accepting_hashkey_from_conn(conn->quic);
        int r;
        khiter_t iter = kh_put_h2o_http3_unauthmap(conn->ctx->conns_accepting, key, &r);
        assert(iter != kh_end(conn->ctx->conns_accepting));
        kh_val(conn->ctx->conns_accepting, iter) = conn;
    }

    if ((ret = open_egress_unistream(conn, &conn->_control_streams.egress.control, h2o_iovec_init(H2O_STRLIT("C\0\4")))) != 0 ||
        (ret = open_egress_unistream(conn, &conn->_control_streams.egress.qpack_encoder, h2o_iovec_init(H2O_STRLIT("H")))) != 0 ||
        (ret = open_egress_unistream(conn, &conn->_control_streams.egress.qpack_decoder, h2o_iovec_init(H2O_STRLIT("h")))) != 0)
        return ret;

    h2o_http3_schedule_timer(conn);
    return 0;
}

void h2o_http3_send(h2o_http3_conn_t *conn)
{
    quicly_datagram_t *packets[16];
    size_t num_packets, i;
    int fd = h2o_socket_get_fd(conn->ctx->sock);

    do {
        num_packets = sizeof(packets) / sizeof(packets[0]);
        int ret = quicly_send(conn->quic, packets, &num_packets);
        switch (ret) {
        case 0:
            for (i = 0; i != num_packets; ++i) {
                if (send_one(fd, packets[i]) == -1)
                    perror("sendmsg failed");
                conn->ctx->quic->packet_allocator->free_packet(conn->ctx->quic->packet_allocator, packets[i]);
            }
            break;
        case QUICLY_ERROR_FREE_CONNECTION:
            conn->callbacks->destroy_connection(conn);
            return;
        default:
            fprintf(stderr, "quicly_send returned %d\n", ret);
            abort();
        }
    } while (num_packets == sizeof(packets) / sizeof(packets[0]));

    h2o_http3_schedule_timer(conn);
}

int h2o_http3_update_recvbuf(h2o_buffer_t **buf, size_t off, const void *src, size_t len)
{
    size_t new_size = off + len;

    if ((*buf)->size < new_size) {
        h2o_buffer_reserve(buf, new_size);
        if ((*buf)->capacity < new_size)
            return PTLS_ERROR_NO_MEMORY;
    }

    memcpy((*buf)->bytes + off, src, len);
    (*buf)->size = new_size;

    return 0;
}

void h2o_http3_schedule_timer(h2o_http3_conn_t *conn)
{
    int64_t timeout = quicly_get_first_timeout(conn->quic);
    if (h2o_timer_is_linked(&conn->_timeout)) {
#if !H2O_USE_LIBUV /* optimization to skip registering a timer specifying the same time */
        if (timeout == conn->_timeout.expire_at)
            return;
#endif
        h2o_timer_unlink(&conn->_timeout);
    }
    uint64_t now = h2o_now(conn->ctx->loop), delay = now < timeout ? timeout - now : 0;
    h2o_timer_link(conn->ctx->loop, delay, &conn->_timeout);
}

int h2o_http3_handle_settings_frame(h2o_http3_conn_t *conn, const uint8_t *payload, size_t length, const char **err_desc)
{
    const uint8_t *src = payload, *src_end = src + length;
    uint32_t header_table_size = H2O_HTTP3_DEFAULT_HEADER_TABLE_SIZE;

    assert(!h2o_http3_has_received_settings(conn));

    while (src != src_end) {
        uint16_t id;
        uint64_t value;
        if (ptls_decode16(&id, &src, src_end) != 0)
            goto Malformed;
        if ((value = quicly_decodev(&src, src_end)) == UINT64_MAX)
            goto Malformed;
        switch (id) {
        case H2O_HTTP3_SETTINGS_HEADER_TABLE_SIZE:
            header_table_size = (uint32_t)value;
            break;
        /* TODO add */
        default:
            break;
        }
    }

    conn->qpack.enc = h2o_qpack_create_encoder(header_table_size, 100 /* FIXME */);
    return 0;
Malformed:
    return H2O_HTTP3_ERROR_MALFORMED_FRAME(H2O_HTTP3_FRAME_TYPE_SETTINGS);
}

void h2o_http3_send_qpack_stream_cancel(h2o_http3_conn_t *conn, quicly_stream_id_t stream_id)
{
    struct st_h2o_http3_egress_unistream_t *stream = conn->_control_streams.egress.qpack_decoder;

    /* allocate and write */
    h2o_iovec_t buf = h2o_buffer_reserve(&stream->sendbuf, stream->sendbuf->size + H2O_HPACK_ENCODE_INT_MAX_LENGTH);
    assert(buf.base != NULL);
    stream->sendbuf->size += h2o_qpack_decoder_send_stream_cancel(conn->qpack.dec, (uint8_t *)buf.base, stream_id);

    /* notify the transport */
    H2O_HTTP3_CHECK_SUCCESS(quicly_stream_sync_sendbuf(stream->quic, 1) == 0);
}

void h2o_http3_send_qpack_header_ack(h2o_http3_conn_t *conn, const void *bytes, size_t len)
{
    struct st_h2o_http3_egress_unistream_t *stream = conn->_control_streams.egress.qpack_encoder;

    assert(stream != NULL);
    H2O_HTTP3_CHECK_SUCCESS(h2o_buffer_append(&stream->sendbuf, bytes, len));
    H2O_HTTP3_CHECK_SUCCESS(quicly_stream_sync_sendbuf(stream->quic, 1));
}