const qInput = document.getElementById("q");
qInput.addEventListener("keydown", e => {
  if (e.key === "Enter") doSearch();
});

// escapes text for safe insertion into html
function escapeHtml(s) {
  return String(s)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;");
}

// escapes regex special characters in a search word
function escapeRegex(s) {
  return s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}

// wraps a matching span of text in a mark tag, escaping everything else
function applyHighlight(text, regex) {
  const parts = text.split(regex);
  let html = "";
  for (let i = 0; i < parts.length; i++) {
    if (i % 2 === 1) {
      html += `<mark>${escapeHtml(parts[i])}</mark>`;
    } else {
      html += escapeHtml(parts[i]);
    }
  }
  return html;
}

// highlights the words only when they appear together as one phrase
function highlightPhrase(text, words) {
  if (!text || !words || words.length === 0) return escapeHtml(text);
  const pattern = words.map(escapeRegex).join("\\W+");
  const regex = new RegExp(`\\b(${pattern})\\b`, "gi");
  return applyHighlight(text, regex);
}

// highlights each query word wherever it appears on its own
function highlightWords(text, words) {
  if (!text || !words || words.length === 0) return escapeHtml(text);
  const pattern = words.map(escapeRegex).join("|");
  const regex = new RegExp(`\\b(${pattern})\\b`, "gi");
  return applyHighlight(text, regex);
}

// builds the html for one line of an occurrence list
function occurrenceRow(lineNum, highlightedHtml, hidden) {
  const hiddenAttr = hidden ? 'style="display:none;"' : "";
  return `
    <div class="occurrence" ${hiddenAttr}>
      <span class="line-badge">line ${lineNum}</span>
      <span class="line-text">${highlightedHtml}</span>
    </div>`;
}

// builds the footer with prev/next buttons, only if more than 3 lines
function paginationFooter(totalLines) {
  if (totalLines <= 3) return "";
  return `
    <div class="card-footer">
      <button class="nav-btn btn-prev" onclick="navigateCard(this, -1)" disabled>&lt;</button>
      <span class="page-info">1-${Math.min(3, totalLines)} of ${totalLines}</span>
      <button class="nav-btn btn-next" onclick="navigateCard(this, 1)">&gt;</button>
    </div>`;
}

// builds one result card for a phrase hit
function phraseHitCard(result, queryWords) {
  let html = `
    <div class="card" data-page="0">
      <div class="card-header">
        <span class="doc-name">${escapeHtml(result.filename)}</span>
        <div class="doc-meta">
          <span class="tag">doc #${result.doc_id}</span>
          <span class="tag">${result.hit_count} hit${result.hit_count !== 1 ? "s" : ""}</span>
        </div>
      </div>`;

  result.hits.forEach((hit, idx) => {
    const highlighted = highlightPhrase(hit.full_line, queryWords);
    html += occurrenceRow(hit.line_number, highlighted, idx >= 3);
  });

  html += paginationFooter(result.hits.length);
  html += `</div>`;
  return html;
}

// builds one result card for a ranked (tf-idf) hit
function rankedHitCard(result, queryWords) {
  const occurrences = result.occurrences || [];

  let html = `
    <div class="card" data-page="0">
      <div class="card-header">
        <span class="doc-name">${escapeHtml(result.filename)}</span>
        <div class="doc-meta">
          <span class="tag">doc #${result.doc_id}</span>
          <span class="tag">score ${result.score.toFixed(3)}</span>
        </div>
      </div>`;

  occurrences.forEach((occ, idx) => {
    const highlighted = highlightWords(occ.full_line, queryWords);
    html += occurrenceRow(occ.line_number, highlighted, idx >= 3);
  });

  html += paginationFooter(occurrences.length);
  html += `</div>`;
  return html;
}

// builds the summary line at the top of the results area
function metaLine(data, phraseHits, rankedHits) {
  let line = `<div class="meta">
    <strong>${rankedHits.length}</strong> document${rankedHits.length !== 1 ? "s" : ""} matched
    &nbsp;|&nbsp;
    <strong>${data.total_docs}</strong> docs in index`;

  if (phraseHits.length) {
    line += `&nbsp;|&nbsp; <strong>${phraseHits.length}</strong> exact phrase hit${phraseHits.length !== 1 ? "s" : ""}`;
  }

  line += `</div>`;
  return line;
}

// renders the phrase_hits section, or a "no match" message
function renderPhraseSection(phraseHits, query, queryWords) {
  if (queryWords.length <= 1) return "";

  if (phraseHits.length === 0) {
    return `<div class="section-header">
      <span class="pill-phrase">exact phrase</span>
      no documents contain "${escapeHtml(query)}" as a consecutive phrase
    </div>`;
  }

  let html = `<div class="section-header">
    <span class="pill-phrase">exact phrase</span>
    found in ${phraseHits.length} document${phraseHits.length !== 1 ? "s" : ""}
  </div>`;

  for (const result of phraseHits) {
    html += phraseHitCard(result, queryWords);
  }
  return html;
}

// renders the ranked_hits section, or a "no match" message
function renderRankedSection(rankedHits, queryWords) {
  let html = `<div class="section-header">
    <span class="pill-ranked">ranked results</span>
    ${rankedHits.length} match${rankedHits.length !== 1 ? "es" : ""} by tf-idf
  </div>`;

  if (rankedHits.length === 0) {
    html += `<div class="state-msg">no documents contain any of the query terms</div>`;
    return html;
  }

  for (const result of rankedHits) {
    html += rankedHitCard(result, queryWords);
  }
  return html;
}

// renders the full results area from the search response
function renderResults(data, query) {
  const area = document.getElementById("results-area");
  const queryWords = query.toLowerCase().split(/\s+/).filter(Boolean);
  const phraseHits = data.phrase_hits || [];
  const rankedHits = data.ranked_hits || [];

  let html = metaLine(data, phraseHits, rankedHits);
  html += renderPhraseSection(phraseHits, query, queryWords);
  html += renderRankedSection(rankedHits, queryWords);

  area.innerHTML = html;
}

// runs a search and renders the result, or an error message
async function doSearch() {
  const query = qInput.value.trim();
  if (!query) return;

  const area = document.getElementById("results-area");
  area.innerHTML = `<div class="loading-row">
    <span class="spinner"></span> searching for "${escapeHtml(query)}"...
  </div>`;

  try {
    const res = await fetch(`/search?q=${encodeURIComponent(query)}`);
    const data = await res.json();

    if (data.error) {
      area.innerHTML = `<p class="error-msg">${escapeHtml(data.error)}</p>`;
      return;
    }
    renderResults(data, query);
  } catch (err) {
    area.innerHTML = `<p class="error-msg">network error: ${escapeHtml(err.message)}</p>`;
  }
}

// asks the server to rebuild the index from the data folder
async function rebuildIndex() {
  const area = document.getElementById("results-area");
  area.innerHTML = `<div class="loading-row">
    <span class="spinner"></span> rebuilding index from data/...
  </div>`;

  try {
    const res = await fetch("/rebuild", { method: "POST" });
    const data = await res.json();

    if (data.status === "ok") {
      area.innerHTML = `<div class="state-msg">index rebuilt, ready to search</div>`;
    } else {
      area.innerHTML = `<p class="error-msg">${escapeHtml(data.message)}</p>`;
    }
  } catch (err) {
    area.innerHTML = `<p class="error-msg">${escapeHtml(err.message)}</p>`;
  }
}

// switches a result card between pages of 3 occurrences
function navigateCard(button, direction) {
  const card = button.closest(".card");
  const occurrences = card.querySelectorAll(".occurrence");
  const total = occurrences.length;
  const maxPages = Math.ceil(total / 3);

  let page = parseInt(card.getAttribute("data-page") || "0");
  page += direction;
  if (page < 0) page = 0;
  if (page >= maxPages) page = maxPages - 1;
  card.setAttribute("data-page", page);

  const startIdx = page * 3;
  const endIdx = startIdx + 3;

  occurrences.forEach((occ, idx) => {
    occ.style.display = (idx >= startIdx && idx < endIdx) ? "grid" : "none";
  });

  card.querySelector(".page-info").textContent =
    `${startIdx + 1}-${Math.min(endIdx, total)} of ${total}`;
  card.querySelector(".btn-prev").disabled = (page === 0);
  card.querySelector(".btn-next").disabled = (page >= maxPages - 1);
}