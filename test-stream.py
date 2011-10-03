#!/usr/bin/env python
import unittest
import subprocess
import re
import os
import sys; sys.path.append('QMP/')
import qmp

def qemu_img(*args):
    devnull = open('/dev/null', 'r+')
    return subprocess.call(['./qemu-img'] + list(args), stdin=devnull, stdout=devnull)

def qemu_io(*args):
    args = ['./qemu-io'] + list(args)
    return subprocess.Popen(args, stdout=subprocess.PIPE).communicate()[0]

class VM(object):
    def __init__(self):
        self._monitor_path = '/tmp/qemu-mon.%d' % os.getpid()
        self._qemu_log_path = '/tmp/qemu-log.%d' % os.getpid()
        self._args = ['x86_64-softmmu/qemu-system-x86_64',
                      '-chardev', 'socket,id=mon,path=' + self._monitor_path,
                      '-mon', 'chardev=mon,mode=control', '-nographic']
        self._num_drives = 0

    def add_drive(self, path, opts=''):
        options = ['if=virtio',
                   'cache=none',
                   'file=%s' % path,
                   'id=drive%d' % self._num_drives]
        if opts:
            options.append(opts)

        self._args.append('-drive')
        self._args.append(','.join(options))
        self._num_drives += 1
        return self

    def launch(self):
        devnull = open('/dev/null', 'rb')
        qemulog = open(self._qemu_log_path, 'wb')
        self._qmp = qmp.QEMUMonitorProtocol(self._monitor_path, server=True)
        self._popen = subprocess.Popen(self._args, stdin=devnull, stdout=qemulog,
                                       stderr=subprocess.STDOUT)
        self._qmp.accept()

    def shutdown(self):
        self._qmp.cmd('quit')
        self._popen.wait()
        os.remove(self._monitor_path)
        os.remove(self._qemu_log_path)

    def qmp(self, cmd, **args):
        return self._qmp.cmd(cmd, args=args)

    def get_qmp_events(self, wait=False):
        events = self._qmp.get_events(wait=wait)
        self._qmp.clear_events()
        return events

index_re = re.compile(r'([^\[]+)\[([^\]]+)\]')

class QMPTestCase(unittest.TestCase):
    def dictpath(self, d, path):
        """Traverse a path in a nested dict"""
        for component in path.split('/'):
            m = index_re.match(component)
            if m:
                component, idx = m.groups()
                idx = int(idx)

            if not isinstance(d, dict) or component not in d:
                self.fail('failed path traversal for "%s" in "%s"' % (path, str(d)))
            d = d[component]

            if m:
                if not isinstance(d, list):
                    self.fail('path component "%s" in "%s" is not a list in "%s"' % (component, path, str(d)))
                try:
                    d = d[idx]
                except IndexError:
                    self.fail('invalid index "%s" in path "%s" in "%s"' % (idx, path, str(d)))
        return d

    def assert_qmp(self, d, path, value):
        result = self.dictpath(d, path)
        self.assertEqual(result, value, 'values not equal "%s" and "%s"' % (str(result), str(value)))

    def assert_no_active_streams(self):
        result = self.vm.qmp('query-block-jobs')
        self.assert_qmp(result, 'return', [])

class TestSingleDrive(QMPTestCase):
    image_len = 1 * 1024 * 1024 # MB

    def setUp(self):
        qemu_img('create', 'backing.img', str(TestSingleDrive.image_len))
        qemu_img('create', '-f', 'qed', '-o', 'backing_file=backing.img', 'test.qed')
        self.vm = VM().add_drive('test.qed')
        self.vm.launch()

    def tearDown(self):
        self.vm.shutdown()
        os.remove('test.qed')
        os.remove('backing.img')

    def test_stream(self):
        self.assert_no_active_streams()

        result = self.vm.qmp('block_stream', device='drive0')
        self.assert_qmp(result, 'return', {})

        completed = False
        while not completed:
            for event in self.vm.get_qmp_events(wait=True):
                if event['event'] == 'BLOCK_JOB_COMPLETED':
                    self.assert_qmp(event, 'data/type', 'stream')
                    self.assert_qmp(event, 'data/device', 'drive0')
                    self.assert_qmp(event, 'data/offset', self.image_len)
                    self.assert_qmp(event, 'data/len', self.image_len)
                    completed = True

        self.assert_no_active_streams()

        self.assertFalse('sectors not allocated' in qemu_io('-c', 'map', 'test.qed'),
                         'image file not fully populated after streaming')

    def test_device_not_found(self):
        result = self.vm.qmp('block_stream', device='nonexistent')
        self.assert_qmp(result, 'error/class', 'DeviceNotFound')

class TestStreamStop(QMPTestCase):
    image_len = 8 * 1024 * 1024 * 1024 # GB

    def setUp(self):
        qemu_img('create', 'backing.img', str(TestStreamStop.image_len))
        qemu_img('create', '-f', 'qed', '-o', 'backing_file=backing.img', 'test.qed')
        self.vm = VM().add_drive('test.qed')
        self.vm.launch()

    def tearDown(self):
        self.vm.shutdown()
        os.remove('test.qed')
        os.remove('backing.img')

    def test_stream_stop(self):
        import time

        self.assert_no_active_streams()

        result = self.vm.qmp('block_stream', device='drive0')
        self.assert_qmp(result, 'return', {})

        time.sleep(1)
        events = self.vm.get_qmp_events(wait=False)
        self.assertEqual(events, [], 'unexpected QMP event: %s' % events)

        self.vm.qmp('block_job_cancel', device='drive0')
        self.assert_qmp(result, 'return', {})

        cancelled = False
        while not cancelled:
            for event in self.vm.get_qmp_events(wait=True):
                if event['event'] == 'BLOCK_JOB_CANCELLED':
                    self.assert_qmp(event, 'data/type', 'stream')
                    self.assert_qmp(event, 'data/device', 'drive0')
                    cancelled = True

        self.assert_no_active_streams()

class TestSetSpeed(QMPTestCase):
    image_len = 80 * 1024 * 1024 # MB

    def setUp(self):
        qemu_img('create', 'backing.img', str(TestSetSpeed.image_len))
        qemu_img('create', '-f', 'qed', '-o', 'backing_file=backing.img', 'test.qed')
        self.vm = VM().add_drive('test.qed')
        self.vm.launch()

    def tearDown(self):
        self.vm.shutdown()
        os.remove('test.qed')
        os.remove('backing.img')

    # This doesn't print or verify anything, only use it via "test-stream.py TestSetSpeed"
    def test_stream_set_speed(self):
        self.assert_no_active_streams()

        result = self.vm.qmp('block_stream', device='drive0')
        self.assert_qmp(result, 'return', {})

        result = self.vm.qmp('block_job_set_speed', device='drive0', value=8 * 1024 * 1024)
        self.assert_qmp(result, 'return', {})

        completed = False
        while not completed:
            for event in self.vm.get_qmp_events(wait=True):
                if event['event'] == 'BLOCK_JOB_COMPLETED':
                    self.assert_qmp(event, 'data/type', 'stream')
                    self.assert_qmp(event, 'data/device', 'drive0')
                    self.assert_qmp(event, 'data/offset', self.image_len)
                    self.assert_qmp(event, 'data/len', self.image_len)
                    completed = True

        self.assert_no_active_streams()

if __name__ == '__main__':
    unittest.main()
