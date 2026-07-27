#!/usr/bin/env python3
"""
Reads recovery records from a FreeSWITCH SQLite DB and sends them
to another FreeSWITCH instance via ESL for call restoration.

The destination instance recovers the channels itself: its core listens for the
recovery::external events sent below.

Usage:
    python3 recovery_migrate.py [options]

    --db        path to source SQLite DB (default: /usr/local/freeswitch/db/core.db)
    --host      ESL host of destination FreeSWITCH (default: 127.0.0.1)
    --port      ESL port (default: 8021)
    --password  ESL password (default: ClueCon)
"""

import sqlite3
import socket
import sys
import argparse

DEFAULT_DB       = '/usr/local/freeswitch/db/core.db'
DEFAULT_HOST     = '127.0.0.1'
DEFAULT_PORT     = 8021
DEFAULT_PASSWORD = 'ClueCon'


def recv_packet(sock):
    """Read one ESL packet (headers + optional body)."""
    buf = b''
    while b'\n\n' not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            raise ConnectionError('ESL connection closed')
        buf += chunk

    head, _, rest = buf.partition(b'\n\n')
    headers = {}
    for line in head.decode().splitlines():
        if ': ' in line:
            k, _, v = line.partition(': ')
            headers[k.strip()] = v.strip()

    body = b''
    if 'Content-Length' in headers:
        need = int(headers['Content-Length'])
        body = rest
        while len(body) < need:
            body += sock.recv(need - len(body))

    return headers, body.decode('utf-8', errors='replace')


def esl_connect(host, port, password):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(10)
    sock.connect((host, port))

    headers, _ = recv_packet(sock)
    if headers.get('Content-Type') != 'auth/request':
        raise ConnectionError(f'Unexpected greeting: {headers}')

    sock.sendall(f'auth {password}\n\n'.encode())
    headers, _ = recv_packet(sock)
    reply = headers.get('Reply-Text', '')
    if not reply.startswith('+OK'):
        raise ConnectionError(f'Auth failed: {reply}')

    return sock


def send_recovery_event(sock, technology, xml_cdr):
    body = xml_cdr.encode('utf-8')
    cmd = (
        f'sendevent CUSTOM\n'
        f'Event-Subclass: recovery::external\n'
        f'Recovery-Technology: {technology}\n'
        f'Content-Type: text/plain\n'
        f'Content-Length: {len(body)}\n'
        f'\n'
    ).encode() + body

    sock.sendall(cmd)

    headers, _ = recv_packet(sock)
    reply = headers.get('Reply-Text', '')
    return reply.startswith('+OK')


def main():
    parser = argparse.ArgumentParser(description='Migrate FreeSWITCH recovery calls via ESL')
    parser.add_argument('--db',       default=DEFAULT_DB,       help='Path to source SQLite DB')
    parser.add_argument('--host',     default=DEFAULT_HOST,     help='ESL host')
    parser.add_argument('--port',     type=int, default=DEFAULT_PORT, help='ESL port')
    parser.add_argument('--password', default=DEFAULT_PASSWORD, help='ESL password')
    args = parser.parse_args()

    con = sqlite3.connect(args.db)
    try:
        rows = con.execute(
            'SELECT uuid, technology, metadata FROM recovery'
        ).fetchall()
    finally:
        con.close()

    if not rows:
        print('No recovery records found.')
        return

    print(f'Found {len(rows)} record(s). Connecting to {args.host}:{args.port} ...')

    try:
        sock = esl_connect(args.host, args.port, args.password)
    except Exception as e:
        print(f'ESL connection failed: {e}', file=sys.stderr)
        sys.exit(1)

    ok = fail = 0
    for uuid, technology, xml_cdr in rows:
        try:
            if send_recovery_event(sock, technology, xml_cdr):
                print(f'  OK  {uuid}  [{technology}]')
                ok += 1
            else:
                print(f'  FAIL {uuid}  [{technology}]')
                fail += 1
        except Exception as e:
            print(f'  ERR {uuid}: {e}', file=sys.stderr)
            fail += 1

    sock.close()
    print(f'\nDone: {ok} ok, {fail} failed.')


if __name__ == '__main__':
    main()
