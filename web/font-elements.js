import { DistanceTextRenderer } from './font-renderer-core.js';

class DistanceTextElement extends HTMLElement {
  constructor(mode) {
    super();
    this.mode = mode;
    this.attachShadow({ mode: 'open' });
    this.shadowRoot.innerHTML = `
      <style>
        :host { display: block; width: 100%; height: 180px; background: #071221; border: 1px solid #1e2c47; border-radius: 10px; overflow: hidden; }
        canvas { display: block; width: 100%; height: 100%; }
      </style>
      <canvas></canvas>
    `;

    this.canvas = this.shadowRoot.querySelector('canvas');
    this.renderer = new DistanceTextRenderer(this.canvas, mode);
    this._textObserver = new MutationObserver(() => this.syncText());
    this._resizeObserver = new ResizeObserver(() => this.renderer.requestDraw());
  }

  static get observedAttributes() {
    return ['size', 'color', 'aa', 'effect', 'stroke', 'glow', 'shadow-x', 'shadow-y', 'text'];
  }

  connectedCallback() {
    this._textObserver.observe(this, { childList: true, characterData: true, subtree: true });
    this._resizeObserver.observe(this);
    this.syncAll();
  }

  disconnectedCallback() {
    this._textObserver.disconnect();
    this._resizeObserver.disconnect();
  }

  attributeChangedCallback() {
    this.syncAll();
  }

  setFontData(data) {
    this.renderer.setFontData(data);
    this.syncAll();
  }

  syncText() {
    this.renderer.setText(this.getAttribute('text') || this.textContent || '');
  }

  syncOptions() {
    this.renderer.setOptions({
      size: Number(this.getAttribute('size') || 56),
      color: this.getAttribute('color') || '#d5fcf0',
      aa: Number(this.getAttribute('aa') || 8),
      effect: this.getAttribute('effect') || 'fill',
      stroke: Number(this.getAttribute('stroke') || 2.5),
      glow: Number(this.getAttribute('glow') || 2),
      shadowX: Number(this.getAttribute('shadow-x') || 4),
      shadowY: Number(this.getAttribute('shadow-y') || -4)
    });
  }

  syncAll() {
    this.syncText();
    this.syncOptions();
  }
}

class MsdfTextElement extends DistanceTextElement {
  constructor() {
    super('msdf');
  }
}

class MtsdfTextElement extends DistanceTextElement {
  constructor() {
    super('mtsdf');
  }
}

customElements.define('msdf-text', MsdfTextElement);
customElements.define('mtsdf-text', MtsdfTextElement);
