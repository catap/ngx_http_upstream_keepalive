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

my $t = Test::Nginx->new()->has('rewrite')->has_daemon('memcached')->plan(10)
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
        keepalive 1;
    }

    upstream memd2 {
        server 127.0.0.1:8081;
        server 127.0.0.1:8082;
        keepalive 1 single;
    }

    upstream memd3 {
        server 127.0.0.1:8081;
        server 127.0.0.1:8082;
        keepalive 1;
    }

    upstream memd4 {
        server 127.0.0.1:8081;
        server 127.0.0.1:8082;
        keepalive 10;
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

        location /memd2 {
            set $memcached_key "/";
            memcached_pass memd2;
        }

        location /memd3 {
            set $memcached_key "/";
            memcached_pass memd3;
        }

        location /memd4 {
            set $memcached_key "/";
            memcached_pass memd4;
        }
    }
}

EOF

$t->run_daemon('memcached', '-l', '127.0.0.1', '-p', '8081');
$t->run_daemon('memcached', '-l', '127.0.0.1', '-p', '8082');
$t->run();

###############################################################################

my $memd1 = Cache::Memcached->new(servers => [ '127.0.0.1:8081' ]);
my $memd2 = Cache::Memcached->new(servers => [ '127.0.0.1:8082' ]);

$memd1->set('/', 'SEE-THIS');
$memd2->set('/', 'SEE-THIS');

my $total = $memd1->stats()->{total}->{total_connections};

like(http_get('/'), qr/SEE-THIS/, 'keepalive memcached request');
like(http_get('/notfound'), qr/404/, 'keepalive memcached not found');
like(http_get('/next'), qr/404/,
	'keepalive not found with memcached_next_upstream');
like(http_get('/'), qr/SEE-THIS/, 'keepalive memcached request again');
like(http_get('/'), qr/SEE-THIS/, 'keepalive memcached request again');
like(http_get('/'), qr/SEE-THIS/, 'keepalive memcached request again');

is($memd1->stats()->{total}->{total_connections}, $total + 1,
	'only one connection used');

# two backends with 'single' option - should establish only one connection

$total = $memd1->stats()->{total}->{total_connections} +
	$memd2->stats()->{total}->{total_connections};

http_get('/memd2');
http_get('/memd2');
http_get('/memd2');

is($memd1->stats()->{total}->{total_connections} +
	$memd2->stats()->{total}->{total_connections}, $total + 1,
	'only one connection with two backends and single');

$total = $memd1->stats()->{total}->{total_connections} +
	$memd2->stats()->{total}->{total_connections};

# two backends without 'single' option and maximum number of cached
# connections set to 1 - should establish new connection on each request

http_get('/memd3');
http_get('/memd3');
http_get('/memd3');

is($memd1->stats()->{total}->{total_connections} +
	$memd2->stats()->{total}->{total_connections}, $total + 3,
	'3 connections should be established');

# two backends without 'single' option and maximum number of cached
# connections set to 10 - should establish only two connections (1 per backend)

$total = $memd1->stats()->{total}->{total_connections} +
        $memd2->stats()->{total}->{total_connections};

http_get('/memd4');
http_get('/memd4');
http_get('/memd4');

is($memd1->stats()->{total}->{total_connections} +
	$memd2->stats()->{total}->{total_connections}, $total + 2,
	'connection per backend');

###############################################################################
