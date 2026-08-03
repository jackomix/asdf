import io
import os
import sys

p = os.path.join(os.path.dirname(__file__), '..', 'kairovm', 'machine.py')
s = open(p).read()

if '_park' in s:
    print('already patched')
    sys.exit(0)

s = s.replace("import struct\nimport sys\nimport time",
              "import struct\nimport sys\nimport threading\nimport time")

s = s.replace("""        self.ctx = None
        self.state = 'new'                # new | ready | running | blocked | done""",
              """        self.ctx = None
        self.state = 'new'                # new | ready | running | blocked | done
        self.ev = threading.Event()
        self.py = None
        self.blocked_on = None
        self.wake_count = 0""")

s = s.replace("""            if self.exit_code is not None:
                break
            if pc == RETURN_MAGIC or pc == 0:
                break""",
              """            if self._yield_flag:
                self._yield_flag = False
                self._park()
                pc = uc.reg_read(A64.UC_ARM64_REG_PC)
                done = 0
                continue
            if self.exit_code is not None:
                break
            if pc == RETURN_MAGIC or pc == 0:
                break""")

s = s.replace("        self.watch = True",
              "        self.watch = True\n        self._yield_flag = False\n"
              "        self._sched_lock = threading.RLock()\n"
              "        self._spurious = 0")

start = s.index("    def spawn(self, entry, arg, name='thread'):")
end = s.index("    def init_array(self, li):")
new = '''    # ------------------------------------------------------- green threads
    #
    # Each guest thread is backed by a host Python thread, but only one is
    # ever inside emu_start(): switching saves/restores the Unicorn CPU
    # context and hands a token to the next runnable thread.  This models
    # exactly what a real OS does for the native R36S build, where these
    # become genuine pthreads.

    def spawn(self, entry, arg, name='thread'):
        t = Thread(self, entry, arg, name)
        t.state = 'ready'
        self.threads.append(t)
        t.py = threading.Thread(target=self._thread_body, args=(t,),
                                name='guest-%d' % t.id, daemon=True)
        t.py.start()
        if self.verbose:
            print('[vm] pthread_create -> tid %d entry=%s' %
                  (t.id, self.describe(entry)))
        return t

    def _thread_body(self, t):
        t.ev.wait()
        t.ev.clear()
        uc = self.uc
        uc.reg_write(A64.UC_ARM64_REG_SP, t.sp)
        uc.reg_write(A64.UC_ARM64_REG_TPIDR_EL0, t.tls_block)
        try:
            t.retval = self.call(t.entry, t.arg)
        except BaseException as e:
            if self.verbose:
                print('[vm] tid %d exited: %s' % (t.id, e), file=sys.stderr)
        t.state = 'done'
        self.wake_object(t, all_=True)
        self._hand_off()

    # ------------------------------------------------------------ blocking
    def block_current(self, obj, note=''):
        """Mark the running thread blocked on `obj` and yield."""
        t = self.current
        t.state = 'blocked'
        t.blocked_on = obj
        if self.verbose > 1:
            print('[vm] tid %d blocks on %r %s' % (t.id, obj, note), file=sys.stderr)
        self._yield_flag = True
        self.uc.emu_stop()

    def yield_current(self):
        self.current.state = 'ready'
        self._yield_flag = True
        self.uc.emu_stop()

    def wake_object(self, obj, n=1, all_=False):
        woke = 0
        for t in self.threads:
            if t.state == 'blocked' and t.blocked_on == obj:
                t.state = 'ready'
                t.blocked_on = None
                woke += 1
                if not all_ and woke >= n:
                    break
        return woke

    def _pick_next(self):
        cur = self.current
        order = self.threads
        i = order.index(cur) if cur in order else -1
        for k in range(1, len(order) + 1):
            t = order[(i + k) % len(order)]
            if t.state == 'ready':
                return t
        # Nothing runnable: hand a spurious wake to a *different* blocked
        # thread if there is one (futex / condvar semantics permit this),
        # otherwise resume the caller.
        for k in range(1, len(order) + 1):
            t = order[(i + k) % len(order)]
            if t.state == 'blocked' and t is not cur:
                t.state = 'ready'
                t.blocked_on = None
                self._spurious += 1
                return t
        return None

    def _park(self):
        """Called on the yielding thread: switch out, wait to be resumed."""
        me = self.current
        nxt = self._pick_next()
        if nxt is None or nxt is me:
            self._spurious += 1
            if self._spurious > 500000:
                raise GuestError('all guest threads deadlocked')
            me.state = 'running'
            return
        me.ctx = self.uc.context_save()
        self._resume(nxt)
        me.ev.wait()
        me.ev.clear()
        self.current = me
        me.state = 'running'

    def _hand_off(self):
        """Called by a thread that will not run again."""
        nxt = self._pick_next()
        if nxt is not None and nxt is not self.current:
            self._resume(nxt)

    def _resume(self, t):
        self.current = t
        t.state = 'running'
        if t.ctx is not None:
            self.uc.context_restore(t.ctx)
            t.ctx = None
        t.ev.set()

    def run_pending_threads(self, max_steps=0):
        """Let every ready thread make progress (used between frames)."""
        for _ in range(len(self.threads)):
            if not any(t.state == 'ready' for t in self.threads
                       if t is not self.current):
                break
            self.yield_current()

'''
s = s[:start] + new + s[end:]
open(p, 'w').write(s)
print('scheduler installed')
