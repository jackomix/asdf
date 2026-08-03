"""Drive the shipped Kairosoft engine: a player loop for the recovered code.

Unity's own player loop lives in libunity.so - the part of the app we cannot
use.  Everything it does that this game depends on is small and well defined:

  * construct the MonoBehaviour that carries the game (main.Main, which
    derives from kairo.unity.ui.IApplication),
  * call Awake / Start once,
  * call Update / LateUpdate / OnGUI once per frame and pump coroutines,
  * hand it input, time, a screen size and a drawing surface.

So we do exactly that, and the shipped ARM64 code of the game runs on top.
"""
import sys
import time
import traceback

from .session import Session
from .machine import GuestError
from .unity import UnityHost, UObj


class Game(object):
    APP_IMAGE = 'Assembly-CSharp.dll'
    APP_NS, APP_NAME = 'main', 'Main'

    def __init__(self, apk='out/apk', width=640, height=480, verbose=1,
                 log_all=False, install=True, platform=11, watch=(),
                 throws=False):
        self.apk = apk
        self.verbose = verbose
        self.s = Session(apk, verbose=verbose)
        self.rt = self.s.rt
        self.m = self.s.m
        self.h = UnityHost(self.s, width, height, verbose=verbose,
                           log_all=log_all)
        self.h.platform = platform
        self.h.prepare()
        if install:
            self.h.install(apk, self.s.meta)
        self.m.enable_method_trace(watch=watch or ())
        if throws:
            self.m.enable_exception_trace(native=12 if int(throws) > 1 else 0)
        self.app = 0                 # managed main.Main
        self.app_class = 0
        self.behaviours = []         # (native, managed, class) started
        self.errors = []
        self._begin_gui = None       # UnityEngine.GUIUtility::BeginGUI
        self._begin_gui_args = None

    # ------------------------------------------------------------- helpers
    def _invoke(self, klass, name, obj, args=(), argc=-1, quiet=False):
        mm = self.rt.method_from_name(klass, name, argc)
        if not mm:
            return None, 'no method %s' % name
        try:
            return self.rt.invoke(mm, obj, args), None
        except GuestError as e:
            if not quiet:
                print('[game] %s.%s raised: %s' % (self.cname(klass), name, e))
                for line in self.m.recent_methods(30):
                    print('    at %s' % line)
            self.errors.append((name, str(e)))
            return None, str(e)

    def cname(self, klass):
        return self.rt.class_name(klass)

    # --------------------------------------------------------------- boot
    def create_app(self):
        """Instantiate the game's MonoBehaviour exactly as Unity would."""
        rt = self.rt
        k = self.s.cls(self.APP_IMAGE, self.APP_NS, self.APP_NAME)
        self.app_class = k
        rt.runtime_class_init(k)
        obj = rt.call('il2cpp_object_new', k)
        self.app = obj

        # native side: a GameObject carrying the behaviour
        go = self.h.make_object('GameObject', 'Kairosoft')
        tf = self.h.make_object('Transform', 'Kairosoft')
        go.transform = tf
        tf.gameobject = go
        self.h.named['Kairosoft'] = go
        comp = UObj(self.h, 'MonoBehaviour', obj, 'main.Main')
        self.h.bind(comp, obj)
        comp.gameobject = go
        comp.transform = tf
        go.components.append(comp)
        self.app_component = comp

        ctor = rt.method_from_name(k, '.ctor', 0)
        if ctor:
            try:
                rt.invoke(ctor, obj, [])
            except GuestError as e:
                print('[game] ctor raised: %s' % e)
        self.h.build_default_skin()
        print('[game] main.Main instance at %#x' % obj)
        return obj

    def hierarchy(self):
        """Print the class chain of the app object (proof it is the real type)."""
        rt, out, k = self.rt, [], self.app_class
        while k:
            out.append('%s.%s' % (rt.class_namespace(k) or '', rt.class_name(k)))
            k = rt.call('il2cpp_class_get_parent', k)
        return ' <- '.join(out)

    # -------------------------------------------------------------- events
    def awake(self):
        return self._invoke(self.app_class, 'Awake', self.app)

    def start(self):
        return self._invoke(self.app_class, 'Start', self.app)

    def frame(self, gui=True):
        h = self.h
        h.frame += 1
        h.now += h.dt
        h.gl = []
        h.render_passes = []             # offscreen camera passes, this frame
        h.draw_calls = 0
        self._invoke(self.app_class, 'Update', self.app)
        self.pump_coroutines()
        self._invoke(self.app_class, 'LateUpdate', self.app, quiet=True)
        if gui:
            # Unity brackets every OnGUI callback with a GUI depth of its own;
            # GUIUtility.CheckOnGUI() reads it before it will draw anything.
            h.gui_depth += 1
            try:
                self.begin_gui()
                self._invoke(self.app_class, 'OnGUI', self.app, quiet=True)
            finally:
                h.gui_depth -= 1
        self.post_frame()
        h.keys_down.clear()
        h.keys_up.clear()
        return h.gl

    def begin_gui(self):
        """Open an IMGUI frame the way Unity's own player loop does.

        GUI.skin is a static the runtime only fills in from
        GUIUtility.ResetGlobalState(), which BeginGUI() calls; without it
        kairo.unity.ui.IApplication.OnGUI dereferences a null skin and gives
        up before it paints.  useGUILayout is 0 because the engine draws
        through its own Canvas/Graphics stack, never GUILayout.

        Event.current has to exist first: OnGUI reads it on its first line.
        """
        self.h.make_event(self.h.event_type)
        if self._begin_gui is None:
            try:
                k = self.s.cls('UnityEngine.IMGUIModule.dll', 'UnityEngine',
                               'GUIUtility')
                self._begin_gui = self.rt.method_from_name(k, 'BeginGUI', 3) or 0
            except KeyError:
                self._begin_gui = 0
        if not self._begin_gui:
            return
        if not self._begin_gui_args:
            # il2cpp_runtime_invoke takes a pointer per value-type argument
            p = self.m.env.alloc(3 * 8)
            for i, v in enumerate((0, 1, 0)):   # skinMode, id, useGUILayout
                self.m.write64(p + i * 8, v)
            self._begin_gui_args = [p, p + 8, p + 16]
        try:
            self.rt.invoke(self._begin_gui, 0, self._begin_gui_args)
        except GuestError as e:
            print('[game] BeginGUI raised: %s' % e)
            self._begin_gui = 0

    def post_frame(self):
        """Everything that needs guest calls but cannot run inside a hook."""
        self.h.resolve_pending()
        while self.h.new_components:
            c = self.h.new_components.pop(0)
            klass = self.m.read64(c.managed)
            for ev in ('Awake', 'Start'):
                self._invoke(klass, ev, c.managed, quiet=True)
            self.behaviours.append(c)

    def pump_coroutines(self):
        """MoveNext() every live coroutine, like the player loop does."""
        if not self.h.coroutines:
            return
        alive = []
        for ent in self.h.coroutines:
            it = ent[0]
            klass = self.m.read64(it)
            mm = self.rt.method_from_name(klass, 'MoveNext', 0)
            if not mm:
                continue
            try:
                if self.rt.invoke(mm, it, []):
                    alive.append(ent)
            except GuestError as e:
                print('[game] coroutine: %s' % e)
        self.h.coroutines = alive

    # ---------------------------------------------------------------- run
    def run(self, frames=1, gui=True):
        for i in range(frames):
            t0 = time.time()
            self.frame(gui=gui)
            if self.verbose:
                print('[game] frame %d: %d draw batches, %.1fs'
                      % (self.h.frame, len(self.h.gl), time.time() - t0))
            if self.s.quit:
                print('[game] application requested quit')
                break

    def report(self):
        print(self.h.report())
        if self.errors:
            print('[game] %d managed errors' % len(self.errors))
            for n, e in self.errors[:20]:
                print('   %-24s %s' % (n, e[:160]))
