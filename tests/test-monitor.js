const { describe, it, before, after } = require('node:test');
const assert = require('node:assert');
const dgram = require('unix-dgram');
const fsp = require('node:fs').promises;
const http = require('node:http');
const monitor = require('../lib/monitor');

const defaultIpcMonitorPath = '/tmp/nodejs.mon';
const ipcMonitorPath = '/tmp/nodejs-test-' + process.pid + '.mon';

const expectedProperties = [
    'cluster',
    'cpu',
    'cpuperreq',
    'elapsed',
    'events',
    'gc',
    'jiffyperreq',
    'kb_trans',
    'kbs_out',
    'mem',
    'oconns',
    'oreqs',
    'pid',
    'reqstotal',
    'rps',
    'sys_cpu',
    'ts',
    'user_cpu',
    'utcstart',
];

const expectedGCCollectors = [
    'marksweep',
    'scavenge',
];

const expectedGCStatProperties = [
    'count',
    'elapsed_ms',
    'max_ms',
];

// Bind a unix datagram socket while temporarily clearing the umask so the
// socket file is world-writable (the monitor sends to it from a C++ thread).
function bindWithOpenUmask(socket, path) {
    const um = process.umask(0);
    socket.bind(path);
    process.umask(um);
}

describe('monitr', function() {
    describe('our own monitor path', function() {
        it('is settable', function() {
            assert.equal(defaultIpcMonitorPath, monitor.ipcMonitorPath);
            monitor.setIpcMonitorPath(ipcMonitorPath);
            assert.equal(ipcMonitorPath, monitor.ipcMonitorPath);
        });
    });

    describe('process.monitor', function() {
        before(function() {
            monitor.setIpcMonitorPath(ipcMonitorPath);
            monitor.start();
        });

        after(function() {
            monitor.stop();
        });

        it('is an object', function() {
            assert.equal(typeof process.monitor, 'object');
        });

        describe('gc property', function() {
            it('is an object', function() {
                assert.equal(typeof process.monitor.gc, 'object');
            });

            it('has count and elapsed properties', function() {
                assert.equal(typeof process.monitor.gc.count, 'number');
                assert.equal(typeof process.monitor.gc.elapsed, 'number');
                function getSetter(obj, prop) {
                    return Object.getOwnPropertyDescriptor(obj, prop).set;
                }
                assert.ok(undefined === getSetter(process.monitor.gc, 'count'));
                assert.ok(undefined === getSetter(process.monitor.gc, 'elapsed'));
                process.monitor.gc.count = 5;
                assert.notEqual(5, process.monitor.gc.count);
                // increase after a GC event
                global.gc();
                assert.notEqual(0, process.monitor.gc.count);
                assert.notEqual(0, process.monitor.gc.elapsed);
            });
        });
    });

    describe('message sent from a server', function() {
        let server, port;
        let socket;
        const topic = {};

        before(async function() {
            monitor.setIpcMonitorPath(ipcMonitorPath);
            monitor.start();

            server = http.createServer(function(req, res) {
                res.writeHead(200, {'Content-Type': 'text/plain'});
                res.end('I am being monitored\n');
            });
            await new Promise((resolve) => server.listen(0, resolve));
            port = server.address().port;

            await new Promise((resolve, reject) => {
                http.get('http://127.0.0.1:' + port, (res) => {
                    res.on('data', () => {});   // drain response body
                    topic.totalRequests = process.monitor.getTotalRequestCount();
                    topic.requests = process.monitor.getRequestCount();
                    topic.openConnections = process.monitor.getOpenConnections();
                    topic.transferred = process.monitor.getTransferred();
                    resolve();
                }).on('error', reject);
            });

            await new Promise((resolve) => {
                socket = dgram.createSocket('unix_dgram', function(msg) {
                    topic.msg = msg.toString();
                    // we just grab one message
                    resolve();
                });
                bindWithOpenUmask(socket, monitor.ipcMonitorPath);
            });
        });

        after(async function() {
            socket.close();
            await new Promise((resolve) => server.close(resolve));
            await fsp.unlink(monitor.ipcMonitorPath);
        });

        it('should return values', function() {
            assert.equal(1, topic.totalRequests);
            assert.equal(0, topic.requests);
            assert.notEqual(0, topic.transferred);
        });

        it('should have valid JSON', function() {
            const msg = JSON.parse(topic.msg);
            assert.ok(Object.prototype.hasOwnProperty.call(msg, 'status'));
            const status = msg.status;
            assert.deepEqual(expectedProperties.sort(), Object.keys(status).sort());
            const gc = status.gc;
            assert.deepEqual(expectedGCCollectors.sort(), Object.keys(gc).sort());
            Object.keys(gc).forEach((coll) => {
                const fields = gc[coll];
                assert.deepEqual(expectedGCStatProperties.sort(), Object.keys(fields).sort());
            });
        });
    });

    describe('health check values', function() {
        let socket;
        const topic = {};

        before(async function() {
            monitor.setIpcMonitorPath(ipcMonitorPath);
            monitor.start();
            process.monitor.setHealthStatus(true, 200);

            await new Promise((resolve) => {
                socket = dgram.createSocket('unix_dgram', function(msg) {
                    topic.isDown = process.monitor.isDown();
                    topic.statusCode = process.monitor.getStatusCode();
                    topic.statusTimestamp = process.monitor.getStatusTimestamp();
                    topic.statusDate = process.monitor.getStatusDate();
                    topic.msg = JSON.parse(msg.toString());
                    // wait for one with health status
                    if (topic.msg.status.health_status_code) {
                        resolve();
                    }
                });
                bindWithOpenUmask(socket, monitor.ipcMonitorPath);
            });
        }, { timeout: 20000 });

        after(async function() {
            socket.close();
            await fsp.unlink(monitor.ipcMonitorPath);
        });

        it('has valid information', function() {
            assert.equal(true, topic.isDown);
            assert.equal(200, topic.statusCode);
            assert.equal(200, topic.msg.status.health_status_code);
            assert.equal(true, topic.msg.status.health_is_down);
            assert.equal(topic.statusTimestamp, topic.msg.status.health_status_timestamp);
            assert.ok(Date.now() - topic.statusDate.getTime() <= 2 * 60 * 1000);  // less than 2 min
            process.monitor.setHealthStatus(false, 300);
            assert.equal(false, process.monitor.isDown());
            assert.equal(300, process.monitor.getStatusCode());
            assert.ok(process.monitor.getStatusTimestamp() > topic.statusTimestamp);
        });
    });
});
