#pylint: disable=missing-docstring
import os
import time
import argparse

class TestIof():
    """IOF filesystem tests in private access mode"""

    startdir = None
    ctrl_dir = None

    def __init__(self, cnss_prefix):
        self.startdir = cnss_prefix
        self.ctrl_dir = self.startdir
        self.ctrl_dir = os.path.join(self.ctrl_dir, ".ctrl")

    def iof_fs_test(self):
        """Test private access mount points"""
        print("starting to stat the mountpoint")
        entry = os.path.join(self.ctrl_dir, "iof", "PA")
        if os.path.isdir(entry):
            for mntfile in os.listdir(entry):
                myfile = os.path.join(entry, mntfile)
                fd = open(myfile, "r")
                mnt_path = fd.readline()
                abs_path = os.path.join(self.startdir, mnt_path)
                stat_obj = os.stat(abs_path)
                print(stat_obj)
            return True

        else:
            print("Mount points not found")
            return False

    def iofstarted(self):
        """Wait for ctrl fs to start"""
        filename = "%s/shutdown" % self.ctrl_dir
        i = 10
        while i > 0:
            i = i - 1
            time.sleep(1)
            if os.path.exists(filename) is True:
                print(os.stat(filename))
                break
        return i

    def iofshutdown(self):
        """Shutdown iof"""
        filename = "%s/shutdown" % self.ctrl_dir
        with open(filename, 'a'):
            os.utime(filename, None)

        return True

if __name__ == "__main__":

    print("Test IOF starting")
    parser = argparse.ArgumentParser(description='IOF Test case Arguments')
    parser.add_argument('cnss_prefix')
    args = parser.parse_args()
    print("CNSS Prefix: %s" % args.cnss_prefix)
    mytest = TestIof(args.cnss_prefix)
    mytest.iofstarted()
    mytest.iof_fs_test()
    mytest.iofshutdown()
