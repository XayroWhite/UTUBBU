const videos = [
  { id:'psp-hardware-test', title:'Test video PSP a 443 MHz — prestazioni reali', channel:'UTUBBU Labs', avatar:'YL', avatarColor:'#7355d9', views:'1,2 Mln di visualizzazioni', age:'2 giorni fa', duration:'0:08', category:'psp tecnologia homebrew recenti', thumb:'thumb-one', src:'media/psp-test-480p.mp4' },
  { id:'psp-2026', title:'PSP nel 2026: ne vale ancora la pena?', channel:'Retro Planet', avatar:'RP', avatarColor:'#167d6b', views:'482.000 visualizzazioni', age:'1 settimana fa', duration:'12:48', category:'psp gaming recenti', thumb:'thumb-two' },
  { id:'convertire-video', title:'Come convertire qualsiasi video per UTUBBU', channel:'UTUBBU Italia', avatar:'YI', avatarColor:'#e0324f', views:'86.000 visualizzazioni', age:'3 settimane fa', duration:'6:21', category:'psp tecnologia homebrew', thumb:'thumb-three' },
  { id:'beats', title:'Beats to relax / game to — PSP edition', channel:'Pocket Sounds', avatar:'PS', avatarColor:'#bd6c21', views:'2,7 Mln di visualizzazioni', age:'1 mese fa', duration:'58:30', category:'musica gaming', thumb:'thumb-four' },
  { id:'homebrew', title:'10 homebrew indispensabili per la tua PSP', channel:'Modding Room', avatar:'MR', avatarColor:'#6847c7', views:'319.000 visualizzazioni', age:'5 giorni fa', duration:'15:02', category:'psp gaming homebrew recenti', thumb:'thumb-five' },
  { id:'ark', title:'ARK-4: configurazione e test completo', channel:'Tech Handheld', avatar:'TH', avatarColor:'#267d91', views:'174.000 visualizzazioni', age:'2 settimane fa', duration:'9:44', category:'psp tecnologia homebrew', thumb:'thumb-six' }
];

const $ = (selector) => document.querySelector(selector);
const results = $('#results');
const search = $('#search');
const clearSearch = $('#clearSearch');
const dialog = $('#playerDialog');
const player = $('#player');
let activeFilter = 'tutti';

function videoCard(item) {
  const article = document.createElement('article');
  article.className = 'video-card';
  article.tabIndex = 0;
  article.dataset.id = item.id;
  article.innerHTML = `
    <div class="thumbnail ${item.thumb}"><span class="play-hover">▶</span><span class="duration">${item.duration}</span></div>
    <div class="video-info">
      <span class="channel-avatar" style="--avatar:${item.avatarColor}">${item.avatar}</span>
      <div class="video-copy"><h3>${item.title}</h3><p>${item.channel} ✓</p><p>${item.views} · ${item.age}</p></div>
      <span class="more">⋮</span>
    </div>`;
  article.addEventListener('click', () => playVideo(item));
  article.addEventListener('keydown', (event) => { if (event.key === 'Enter') playVideo(item); });
  return article;
}

function render() {
  const query = search.value.trim().toLocaleLowerCase('it');
  const matches = videos.filter(item => {
    const inFilter = activeFilter === 'tutti' || item.category.includes(activeFilter);
    return inFilter && `${item.title} ${item.channel} ${item.category}`.toLocaleLowerCase('it').includes(query);
  });
  results.replaceChildren(...matches.map(videoCard));
  if (!matches.length) {
    const empty = document.createElement('div');
    empty.className = 'no-results';
    empty.innerHTML = '<b>Nessun video trovato</b>Prova con parole chiave o filtri diversi.';
    results.append(empty);
  }
  clearSearch.classList.toggle('visible', Boolean(query));
}

function playVideo(item) {
  $('#playerTitle').textContent = item.title;
  $('#playerMeta').textContent = `${item.channel} · ${item.views}`;
  player.src = item.src || videos[0].src;
  dialog.showModal();
  player.play().catch(() => {});
}

$('#searchForm').addEventListener('submit', event => { event.preventDefault(); render(); });
search.addEventListener('input', render);
clearSearch.addEventListener('click', () => { search.value=''; search.focus(); render(); });
$('#chips').addEventListener('click', event => {
  const chip = event.target.closest('.chip');
  if (!chip) return;
  activeFilter = chip.dataset.filter;
  document.querySelectorAll('.chip').forEach(item => item.classList.toggle('active', item === chip));
  render();
});
document.querySelectorAll('[data-play]').forEach(button => button.addEventListener('click', () => playVideo(videos.find(item => item.id === button.dataset.play))));
$('#playerClose').addEventListener('click', () => dialog.close());
dialog.addEventListener('click', event => { if (event.target === dialog) dialog.close(); });
dialog.addEventListener('close', () => { player.pause(); player.currentTime = 0; });
$('#menuButton').addEventListener('click', () => document.body.classList.toggle('sidebar-collapsed'));

const hardwarePanel = $('#hardwarePanel');
$('#hardwareToggle').addEventListener('click', () => hardwarePanel.classList.add('open'));
$('#hardwareClose').addEventListener('click', () => hardwarePanel.classList.remove('open'));
const cpu = $('#cpu');
const bus = $('#bus');
function updateProfile() {
  const cpuMHz = Number(cpu.value);
  const busMHz = Number(bus.value);
  const estimatedLoad = Math.min(100, Math.round(29400 / cpuMHz + 2400 / busMHz));
  $('#screenCpu').textContent = cpuMHz;
  $('#screenBus').textContent = busMHz;
  const meter = $('#load');
  meter.style.width = `${estimatedLoad}%`;
  meter.className = estimatedLoad > 90 ? 'danger' : estimatedLoad > 72 ? 'warn' : 'ok';
  $('#status').textContent = cpuMHz > 333
    ? `Profilo ARK sperimentale · carico stimato ${estimatedLoad}%`
    : cpuMHz === 333
      ? `Profilo massimo standard · carico stimato ${estimatedLoad}%`
      : `Profilo ridotto · carico stimato ${estimatedLoad}%`;
}
cpu.addEventListener('input', updateProfile);
bus.addEventListener('input', updateProfile);

document.querySelector('.create-button').addEventListener('click', () => {
  const toast = $('#toast');
  toast.textContent = 'Caricamento video disponibile nella prossima versione';
  toast.classList.add('show');
  setTimeout(() => toast.classList.remove('show'), 2600);
});

render();
updateProfile();
