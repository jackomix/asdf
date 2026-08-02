/* =========================================================================
 * 55-licensing.js -- Google Play licensing (LVL) service emulation
 *
 * The com.android.vending.licensing.* classes are translated from the DEX
 * like everything else, but they talk to the Play Store over a Binder IPC
 * that simply does not exist outside Android.  Instead of touching the
 * game's own code we emulate the *service response*: kairo.android.h.b.a()
 * is the entry point that asks the licence service, so it now delivers the
 * LICENSED answer straight to the app's LicenseCheckerCallback, exactly as
 * a licensed device would.
 *
 * Effect on the game: kairo.android.f.b sees checked==1 / result==1 and
 * short-circuits its regist.php round trip to the canned "1\n100" reply,
 * i.e. the behaviour of a properly purchased, online copy.
 * ========================================================================= */
'use strict';

(function ($rt) {
  const mangle = $rt.mangle;

  function install() {
    const B = $rt.classes['kairo/android/h/b'];
    if (!B) {
      console.warn('[licensing] kairo/android/h/b not present - patch skipped');
      return;
    }
    const check = mangle('a()V');
    B.prototype[check] = function () {
      /* mirror the field reset the original method performs */
      this.f_c = 0;             // "answer received" flag
      this.f_d = 2;             // result (2 == RETRY / unknown)
      this.f_e = 2147483647;    // last error code

      /* deliver LICENSED through the game's own callback object */
      const cb = this.f_b;      // kairo.android.h.a implements ...licensing.r
      if (cb) $rt.invoke(cb, 'a()V', []);
      else { this.f_c = 1; this.f_d = 1; }
    };

    /* Nothing ever binds to the Play service, so make the checker's
     * connection callbacks inert instead of letting them time out. */
    const G = $rt.classes['com/android/vending/licensing/g'];
    if (G) {
      const noop = function () {};
      const sigs = ['onServiceConnected(Landroid/content/ComponentName;Landroid/os/IBinder;)V',
                    'onServiceDisconnected(Landroid/content/ComponentName;)V'];
      for (const s of sigs) {
        const n = mangle(s);
        if (G.prototype[n]) G.prototype[n] = noop;
      }
    }
    $rt.licensed = true;
  }

  $rt.installLicensePatch = install;
})($rt);
