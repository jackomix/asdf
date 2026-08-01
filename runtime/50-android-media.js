/* =========================================================================
 * 50-android-media.js -- android.media.*
 *
 * MediaPlayer maps 1:1 onto a WebAudio AudioBufferSourceNode.  The ogg
 * files from res/raw are decoded once during boot, so MediaPlayer.create()
 * can stay synchronous like the Android API it replaces.
 * ========================================================================= */
'use strict';

(function ($rt) {
  const def = $rt.def, iface = $rt.iface, mangle = $rt.mangle;

  /* ------------------------------------------------------- audio device */
  const audio = {
    ctx: null,
    master: null,
    buffers: Object.create(null),   // raw resource name -> AudioBuffer
    encoded: Object.create(null),   // raw resource name -> ArrayBuffer
    volume: 1,
    unlocked: false,

    ensure() {
      if (this.ctx) return this.ctx;
      const AC = (typeof AudioContext !== 'undefined') ? AudioContext :
                 (typeof webkitAudioContext !== 'undefined') ? webkitAudioContext : null;
      if (!AC) return null;
      this.ctx = new AC();
      this.master = this.ctx.createGain();
      this.master.gain.value = this.volume;
      this.master.connect(this.ctx.destination);
      return this.ctx;
    },
    resume() {
      const c = this.ensure();
      if (c && c.state === 'suspended') c.resume();
      this.unlocked = true;
      /* decode anything that was queued before the context existed */
      for (const name in this.encoded) this.decode(name);
    },
    decode(name) {
      const c = this.ensure();
      if (!c) return;
      const raw = this.encoded[name];
      if (!raw || this.buffers[name]) return;
      delete this.encoded[name];
      const done = (buf) => { this.buffers[name] = buf; };
      try {
        const p = c.decodeAudioData(raw.slice(0));
        if (p && p.then) p.then(done, (e) => $host.log('[audio] decode failed ' + name));
      } catch (e) {
        c.decodeAudioData(raw.slice(0), done, () => {});
      }
    },
    put(name, arrayBuffer) {
      this.encoded[name] = arrayBuffer;
      if (this.ctx) this.decode(name);
    },
    setMaster(v) {
      this.volume = v;
      if (this.master) this.master.gain.value = v;
    },
  };
  $host.audio = audio;

  /* -------------------------------------------------------- MediaPlayer */
  iface('android/media/MediaPlayer$OnCompletionListener');
  iface('android/media/MediaPlayer$OnPreparedListener');
  iface('android/media/MediaPlayer$OnErrorListener');

  const MediaPlayer = def('android/media/MediaPlayer', null, {
    ctor() {
      this.$name = null; this.$src = null; this.$gain = null;
      this.$vol = 1; this.$loop = false; this.$onDone = null;
      this.$offset = 0; this.$startedAt = 0; this.$playing = false;
      this.$released = false;
    },
    m: {
      '<init>()V': function () { return this; },
      'setOnCompletionListener(Landroid/media/MediaPlayer$OnCompletionListener;)V':
        function (l) { this.$onDone = l; },
      'setOnPreparedListener(Landroid/media/MediaPlayer$OnPreparedListener;)V':
        function () {},
      'setOnErrorListener(Landroid/media/MediaPlayer$OnErrorListener;)V': function () {},
      'setLooping(Z)V': function (v) {
        this.$loop = !!v;
        if (this.$src) this.$src.loop = this.$loop;
      },
      'isLooping()Z': function () { return this.$loop ? 1 : 0; },
      'isPlaying()Z': function () { return this.$playing ? 1 : 0; },
      'setVolume(FF)V': function (l, r) {
        this.$vol = Math.max(0, Math.min(1, (l + r) / 2));
        if (this.$gain) this.$gain.gain.value = this.$vol;
      },
      'seekTo(I)V': function (ms) {
        const was = this.$playing;
        stop(this);
        this.$offset = ms / 1000;
        if (was) start(this);
      },
      'getCurrentPosition()I': function () {
        if (!this.$playing || !audio.ctx) return Math.round(this.$offset * 1000);
        return Math.round((this.$offset + audio.ctx.currentTime - this.$startedAt) * 1000);
      },
      'getDuration()I': function () {
        const b = audio.buffers[this.$name];
        return b ? Math.round(b.duration * 1000) : 0;
      },
      'start()V': function () { start(this); },
      'pause()V': function () { stop(this); },
      'stop()V': function () { stop(this); this.$offset = 0; },
      'reset()V': function () { stop(this); this.$offset = 0; },
      'prepare()V': function () {},
      'release()V': function () { stop(this); this.$released = true; },
    },
    s: {
      'create(Landroid/content/Context;I)Landroid/media/MediaPlayer;': function (ctx, id) {
        const raw = $host.rawById[id];
        const mp = new MediaPlayer();
        mp.$name = raw ? raw.name : null;
        if (raw) audio.decode(raw.name);
        return mp;
      },
    },
  });

  function start(mp) {
    if (mp.$released) return;
    mp.$playing = true;
    const c = audio.ensure();
    const buf = c && audio.buffers[mp.$name];
    if (!buf) {
      /* audio still decoding (or muted by autoplay policy): remember that we
       * should be playing so the next start()/resume picks it up */
      mp.$pending = true;
      return;
    }
    stopNodes(mp);
    const src = c.createBufferSource();
    src.buffer = buf;
    src.loop = mp.$loop;
    const g = c.createGain();
    g.gain.value = mp.$vol;
    src.connect(g);
    g.connect(audio.master);
    src.onended = () => {
      if (mp.$src !== src) return;
      mp.$playing = false;
      mp.$src = null;
      if (mp.$onDone) {
        $rt.invoke(mp.$onDone, 'onCompletion(Landroid/media/MediaPlayer;)V', [mp]);
        $rt.scheduler.kick();
      }
    };
    try { src.start(0, Math.min(mp.$offset, buf.duration)); } catch (e) { src.start(0); }
    mp.$src = src;
    mp.$gain = g;
    mp.$startedAt = c.currentTime;
    mp.$pending = false;
  }

  function stop(mp) {
    mp.$playing = false;
    if (mp.$src && audio.ctx) {
      mp.$offset += audio.ctx.currentTime - mp.$startedAt;
    }
    stopNodes(mp);
  }

  function stopNodes(mp) {
    const s = mp.$src;
    mp.$src = null;
    if (s) {
      s.onended = null;
      try { s.stop(0); } catch (e) { /* already stopped */ }
      try { s.disconnect(); } catch (e) { /* ignore */ }
    }
    if (mp.$gain) {
      try { mp.$gain.disconnect(); } catch (e) { /* ignore */ }
      mp.$gain = null;
    }
  }

  /* -------------------------------------------------------- AudioManager */
  def('android/media/AudioManager', null, {
    ctor() { this.$vol = [12, 12, 12, 12, 12, 12, 12, 12]; },
    m: {
      'getStreamMaxVolume(I)I': function () { return 15; },
      'getStreamVolume(I)I': function (s) { return this.$vol[s & 7]; },
      'setStreamVolume(III)V': function (s, v, f) {
        v = Math.max(0, Math.min(15, v));
        this.$vol[s & 7] = v;
        if ((s & 7) === 3) audio.setMaster(v / 15);
      },
      'getMode()I': function () { return 0; },
      'setMode(I)V': function () {},
      'isMusicActive()Z': function () { return 0; },
    },
    sf: { STREAM_MUSIC: 3, STREAM_SYSTEM: 1, ADJUST_RAISE: 1, ADJUST_LOWER: -1 },
  });
  $rt.audioManager = new ($rt.classes['android/media/AudioManager'])();

  /* ------------------------------------------------------------ JetPlayer */
  /* The JET engine only handles .jet interactive-MIDI resources.  This build
   * of the game ships nothing but .ogg, so the JET path is never taken; the
   * object still has to exist because kairo.android.ui.y references it.     */
  const JetPlayer = def('android/media/JetPlayer', null, {
    m: {
      'loadJetFile(Landroid/content/res/AssetFileDescriptor;)Z': function () { return 0; },
      'loadJetFile(Ljava/lang/String;)Z': function () { return 0; },
      'closeJetFile()Z': function () { return 1; },
      'clearQueue()Z': function () { return 1; },
      'queueJetSegment(IIIIIB)Z': function () { return 1; },
      'queueJetSegmentMuteArray(IIII[ZB)Z': function () { return 1; },
      'play()Z': function () { return 1; },
      'pause()Z': function () { return 1; },
      'release()V': function () {},
      'setMuteFlags(IZ)Z': function () { return 1; },
    },
    s: {
      'getJetPlayer()Landroid/media/JetPlayer;': function () {
        if (!JetPlayer.$inst) JetPlayer.$inst = new JetPlayer();
        return JetPlayer.$inst;
      },
    },
  });
})($rt);
