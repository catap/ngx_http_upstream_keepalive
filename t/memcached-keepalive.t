#!/usr/bin/perl

# (C) Maxim Dounin

# Test for memcached with keepalive.

###############################################################################

use warnings;
use strict;

use Test::More;
use Test::Nginx;

###############################################################################

select STDERR; $| = 1;
select STDOUT; $| = 1;

eval { require Cache::Memcached; };
plain(skip_all => 'Cache::Memcached not installed') if $@;

my $t = Test::Nginx->new()->has('rewrite')->has_daemon('memcached')->plan(7)
	->write_file_expand('nginx.conf', <<'EOF');

master_process off;
daemon         off;

events {
    worker_connections  1024;
}

http {
    access_log    off;

    client_body_temp_path  %%TESTDIR%%/client_body_temp;
    fastcgi_temp_path      %%TESTDIR%%/fastcgi_temp;
    proxy_temp_path        %%TESTDIR%%/proxy_temp;

    upstream memd {
        server 127.0.0.1:8081;
        keepalive;
    }

    server {
        listen       localhost:8080;
        server_name  localhost;

        location / {
            set $memcached_key $uri;
            memcached_pass memd;
        }

        location /next {
            set $memcached_key $uri;
            memcached_next_upstream  not_found;
            memcached_pass memd;
        }
    }
}

EOF

$t->run_daemon('memcached', '-l', '127.0.0.1', '-p', '8081');
$t->run();

###############################################################################

my $memd = Cache::Memcached->new(servers => [ '127.0.0.1:8081' ]);
$memd->set('/', 'SEE-THIS');

my $total = $memd->stats()->{total}->{total_connections};

like(http_get('/'), qr/SEE-THIS/, 'keepalive memcached request');
like(http_get('/notfound'), qr/404/, 'keepalive memcached not found');
like(http_get('/next'), qr/404/,
	'keepalive not found with memcached_next_upstream');
like(http_get('/'), qr/SEE-THIS/, 'keepalive memcached request again');
like(http_get('/'), qr/SEE-THIS/, 'keepalive memcached request again');
like(http_get('/'), qr/SEE-THIS/, 'keepalive memcached request again');

is($memd->stats()->{total}->{total_connections}, $total + 1, 'keepalive used');

###############################################################################
