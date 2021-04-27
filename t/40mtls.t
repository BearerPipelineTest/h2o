use strict;
use warnings;
use File::Temp qw(tempdir);
use Net::EmptyPort qw(empty_port);
use Test::More;
use t::Util;

my $tls_port = empty_port();
my $good_client_key_cert = '--key t/assets/test_client.key --cert t/assets/test_client.crt';
my $wrong_client_key_cert = '--key examples/h2o/server.key --cert examples/h2o/server.crt';
my $TLS_RE_OK = qr{hello};

# subtest "tls1.2" => sub {
#     my $server = spawn_server("tlsv1.2");
#     run_tests('');
# };
#
# sleep 1; # without this sleep; the server occasionally fails to bind to $tls_port (maybe some sub-process of h2o still hanging
#          # around?)

subtest "tls1.3" => sub {
    # plan skip_all => 'curl does not support TLS 1.3'
    #     unless curl_support_tls13();
    my $server = spawn_server("tlsv1.3");
    run_tests('--tlsv1.3') for 1 .. 100;
};

done_testing;

sub run_tests {
    my $opts = shift;
    like run_tls_client("$opts $good_client_key_cert"), $TLS_RE_OK, "correct client cert" or die "test failed";
    # unlike run_tls_client($opts), $TLS_RE_OK, "no client cert";
    # unlike run_tls_client("$opts $wrong_client_key_cert"), $TLS_RE_OK, "wrong client cert";
}

sub run_tls_client {
    my $opts = shift;
    my $resp = `curl --max-time 1 --http1.1 --silent --show-error $opts --cacert misc/test-ca/root/ca.crt https://127.0.0.1.xip.io:$tls_port`;
    return $resp;
}

sub spawn_server {
    my $tls_max = shift;
    die "invalid arg:$tls_max" unless $tls_max =~ /^tlsv/;
    spawn_h2o_raw(<<"EOT", [ $tls_port ]);
hosts:
  "default":
    paths:
      "/":
        file.dir: @{[DOC_ROOT]}
listen:
  port: $tls_port
  ssl: &ssl
    max-version: $tls_max
    key-file: examples/h2o/server.key
    certificate-file: examples/h2o/server.crt
    client-ca-file: misc/test-ca/intermediate/ca.crt
EOT
}

sub curl_support_tls13 {
    # Unavailability of TLS 1.3 support can be detected only after curl connects to the server. Therefore, we setup a dummy server,
    # run curl, accept a connection, then see what happens
    my $listen = IO::Socket::INET->new(
        LocalAddr => "127.0.0.1:0",
        Proto     => "tcp",
        Listen    => 5,
    ) or die "failed to listen to random port:$!";
    open my $fh, "-|", "curl --silent --show-error --tlsv1.3 https://127.0.0.1:@{[$listen->sockport]}/ 2>&1"
        or die "failed to launch curl:$!";
    $listen->accept;
    sleep 0.5;
    close $listen;
    close $fh;
    $? >> 8 != 4; # exit status 4 indicates missing feature
}
