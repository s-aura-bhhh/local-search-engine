// g++ -std=c++17 -O2 -Wall -Wextra -o src/indexer src/indexer.cpp
// command to build the code

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <thread>
#include <mutex>
#include <chrono>
#include <cstdint>
#include <cassert>
#include <stdexcept>
#include <iomanip>

namespace fs = std::filesystem;

using std::string;
using std::vector;
using std::unordered_map;
using std::unordered_set;
using std::ifstream;
using std::ofstream;
using std::istringstream;
using std::cout;
using std::cerr;
using std::remove_if;
using std::transform;
using std::isalpha;
using std::thread;
using std::mutex;
using std::lock_guard;
using std::sort;
using std::log2;

using RawList      = vector<int>;
using RawPostings  = unordered_map<int, RawList>;
using RawIndex     = unordered_map<string, RawPostings>;
using RawLineIndex = RawIndex;

using CompList     = vector<uint8_t>;
using CompPostings = unordered_map<int, CompList>;
using CompIndex    = unordered_map<string, CompPostings>;
using CompLineIndex = CompIndex;

// keeps track of doc id -> filepath
struct DocRegistry {
    unordered_map<int, string> id_to_path;
    int   next_id{0};
    mutex mtx;

    int add(const string& filepath) {
        lock_guard<mutex> guard(mtx);
        int id = next_id++;
        id_to_path[id] = filepath;
        return id;
    }
    const string& lookup(int id) const { return id_to_path.at(id); }
    int size() const { return static_cast<int>(id_to_path.size()); }
};

// strips non-letters, lowercases
string normalize(string token) {
    token.erase(remove_if(token.begin(), token.end(),
                    [](unsigned char c){ return !isalpha(c); }), token.end());
    transform(token.begin(), token.end(), token.begin(), ::tolower);
    return token;
}

// reads one file, fills word + line postings
void indexFile(const string& filepath, int doc_id,
               RawIndex& index, RawLineIndex& lines) {
    ifstream file(filepath);
    if (!file.is_open()) { cerr << "skip (cannot open): " << filepath << "\n"; return; }

    string raw_line;
    int pos      = 0;
    int line_num = 1;

    while (getline(file, raw_line)) {
        istringstream iss(raw_line);
        string token;
        while (iss >> token) {
            string t = normalize(token);
            if (!t.empty()) {
                index[t][doc_id].push_back(pos);
                lines[t][doc_id].push_back(line_num);
            }
            pos++;
        }
        line_num++;
    }
}

// merges one thread's local index into the shared one
static void mergeOne(const RawIndex& local, RawIndex& global_idx) {
    for (const auto& [word, postings] : local) {
        for (const auto& [doc_id, positions] : postings) {
            global_idx[word][doc_id] = positions;
        }
    }
}

// locked merge of word + line indexes
void mergeLocal(const RawIndex& local_idx,   RawIndex& gidx,
                const RawLineIndex& local_ln, RawLineIndex& glines,
                mutex& mtx) {
    lock_guard<mutex> guard(mtx);
    mergeOne(local_idx, gidx);
    mergeOne(local_ln,  glines);
}

// indexes a batch of files on one thread
void workerThread(const vector<string>& files,
                  RawIndex& local_idx, RawLineIndex& local_ln,
                  DocRegistry& reg) {
    for (const auto& fp : files)
        indexFile(fp, reg.add(fp), local_idx, local_ln);
}

// varint encode/decode for storing position deltas compactly
void encodeVarInt(uint32_t v, CompList& out) {
    do { uint8_t b = v & 0x7F; v >>= 7; if (v) b |= 0x80; out.push_back(b); } while (v);
}
uint32_t decodeVarInt(const CompList& buf, size_t& off) {
    uint32_t v = 0; int sh = 0;
    while (off < buf.size()) {
        uint8_t b = buf[off++];
        v |= static_cast<uint32_t>(b & 0x7F) << sh;
        if (!(b & 0x80)) break;
        sh += 7;
    }
    return v;
}

// delta + varint compress a sorted position list
CompList compress(const RawList& pos) {
    CompList out; out.reserve(pos.size());
    uint32_t prev = 0;
    for (int p : pos) {
        assert(static_cast<uint32_t>(p) >= prev);
        encodeVarInt(static_cast<uint32_t>(p) - prev, out);
        prev = static_cast<uint32_t>(p);
    }
    return out;
}

RawList decompress(const CompList& bytes) {
    RawList out; size_t off = 0; int run = 0;
    while (off < bytes.size()) { run += static_cast<int>(decodeVarInt(bytes, off)); out.push_back(run); }
    return out;
}

// compresses every posting list in the index
CompIndex compressIndex(const RawIndex& raw) {
    CompIndex comp;
    comp.reserve(raw.size());
    for (const auto& [word, postings] : raw) {
        for (const auto& [doc_id, positions] : postings) {
            comp[word][doc_id] = compress(positions);
        }
    }
    return comp;
}

// walks a directory, splits .txt files across threads, indexes them
void indexDirectoryConcurrent(const string& dir,
                               RawIndex& gidx, RawLineIndex& glines,
                               DocRegistry& reg) {
    if (!fs::exists(dir) || !fs::is_directory(dir))
        throw std::runtime_error("not a valid directory: " + dir);

    vector<string> files;
    for (const auto& e : fs::directory_iterator(dir))
        if (e.is_regular_file() && e.path().extension() == ".txt")
            files.push_back(e.path().string());
    if (files.empty()) { cerr << "no .txt files found\n"; return; }

    const int hw = static_cast<int>(thread::hardware_concurrency());
    const int nt = std::max(1, std::min(hw > 0 ? hw : 4,
                                        static_cast<int>(files.size())));
    cerr << "threads: " << nt << "  files: " << files.size() << "\n";

    vector<vector<string>> work(nt);
    for (size_t i = 0; i < files.size(); ++i) work[i % nt].push_back(files[i]);

    vector<RawIndex>     locals(nt);
    vector<RawLineIndex> local_lines(nt);
    for (auto& l : locals)      l.reserve(1 << 14);
    for (auto& l : local_lines) l.reserve(1 << 14);

    vector<thread> threads; threads.reserve(nt);
    auto t0 = std::chrono::steady_clock::now();
    for (int t = 0; t < nt; ++t)
        threads.emplace_back(workerThread,
                             std::cref(work[t]),
                             std::ref(locals[t]),
                             std::ref(local_lines[t]),
                             std::ref(reg));
    for (auto& t : threads) t.join();
    auto t1 = std::chrono::steady_clock::now();

    mutex mmtx;
    gidx.reserve(1 << 17);
    glines.reserve(1 << 17);
    for (int t = 0; t < nt; ++t)
        mergeLocal(locals[t], gidx, local_lines[t], glines, mmtx);
    auto t2 = std::chrono::steady_clock::now();

    using ms = std::chrono::milliseconds;
    cerr << "index time: " << std::chrono::duration_cast<ms>(t1-t0).count()
         << "ms  merge time: "  << std::chrono::duration_cast<ms>(t2-t1).count() << "ms\n";
}

// prints doc/term/posting counts
void printStats(const RawIndex& raw, const DocRegistry& reg) {
    size_t total = 0;
    for (const auto& [word, postings] : raw) {
        for (const auto& [doc_id, positions] : postings) {
            total += positions.size();
        }
    }
    cerr << "docs=" << reg.size() << " terms=" << raw.size()
         << " postings=" << total << "\n";
}

// prints raw vs compressed size
void printMemoryStats(const RawIndex& raw, const CompIndex& comp,
                      const RawLineIndex& raw_ln, const CompLineIndex& comp_ln) {
    size_t raw_word_bytes = 0, comp_word_bytes = 0;
    size_t raw_line_bytes = 0, comp_line_bytes = 0;

    for (const auto& [word, postings] : raw)
        for (const auto& [doc_id, positions] : postings)
            raw_word_bytes += positions.size() * 4;

    for (const auto& [word, postings] : comp)
        for (const auto& [doc_id, bytes] : postings)
            comp_word_bytes += bytes.size();

    for (const auto& [word, postings] : raw_ln)
        for (const auto& [doc_id, positions] : postings)
            raw_line_bytes += positions.size() * 4;

    for (const auto& [word, postings] : comp_ln)
        for (const auto& [doc_id, bytes] : postings)
            comp_line_bytes += bytes.size();

    cerr << "word index: " << raw_word_bytes << "B -> " << comp_word_bytes << "B ("
         << std::fixed << std::setprecision(2)
         << static_cast<double>(raw_word_bytes) / std::max(comp_word_bytes, size_t(1)) << "x)\n";
    cerr << "line index: " << raw_line_bytes << "B -> " << comp_line_bytes << "B ("
         << static_cast<double>(raw_line_bytes) / std::max(comp_line_bytes, size_t(1)) << "x)\n";
}

struct SearchResult { int doc_id; double score; vector<int> positions; };

// merge join of two sorted position lists
RawList intersectSorted(const RawList& a, const RawList& b) {
    RawList out; size_t i=0,j=0;
    while (i<a.size()&&j<b.size()) {
        if (a[i]==b[j]){out.push_back(a[i]);++i;++j;}
        else if(a[i]<b[j])++i; else ++j;
    }
    return out;
}

// pulls back the text of a set of line numbers in one pass over the file
unordered_map<int, string> getLinesSinglePass(const string& filepath, const unordered_set<int>& required_lines) {
    unordered_map<int, string> line_map;
    if (required_lines.empty()) return line_map;

    ifstream file(filepath);
    if (!file.is_open()) return line_map;

    string line;
    int current_line = 1;
    size_t remaining = required_lines.size();

    while (getline(file, line) && remaining > 0) {
        if (required_lines.count(current_line)) {
            line_map[current_line] = line;
            remaining--;
        }
        current_line++;
    }
    return line_map;
}

// finds docs where the query words appear as a consecutive phrase
vector<SearchResult> phraseMatch(const vector<string>& words, const CompIndex& comp) {
    if (words.empty()) return {};

    auto first = comp.find(words[0]);
    if (first == comp.end()) return {};

    unordered_set<int> candidates;
    for (const auto& [doc_id, bytes] : first->second) {
        candidates.insert(doc_id);
    }

    for (size_t k = 1; k < words.size(); ++k) {
        auto it = comp.find(words[k]);
        if (it == comp.end()) return {};

        unordered_set<int> next;
        for (const auto& [doc_id, bytes] : it->second) {
            if (candidates.count(doc_id)) next.insert(doc_id);
        }
        candidates = std::move(next);
        if (candidates.empty()) return {};
    }

    vector<SearchResult> results;
    for (int doc_id : candidates) {
        RawList anchors = decompress(comp.at(words[0]).at(doc_id));

        for (size_t k = 1; k < words.size(); ++k) {
            RawList positions = decompress(comp.at(words[k]).at(doc_id));
            RawList shifted;
            shifted.reserve(positions.size());
            for (int p : positions) {
                int shifted_pos = p - static_cast<int>(k);
                if (shifted_pos >= 0) shifted.push_back(shifted_pos);
            }
            anchors = intersectSorted(anchors, shifted);
            if (anchors.empty()) break;
        }

        if (!anchors.empty()) {
            results.push_back({doc_id, static_cast<double>(anchors.size()), anchors});
        }
    }

    sort(results.begin(), results.end(),
         [](const SearchResult& a, const SearchResult& b) { return a.score > b.score; });
    return results;
}

// ranks docs by tf-idf over the query words
vector<SearchResult> tfidfRank(const vector<string>& words,
                                const CompIndex& comp, int total_docs) {
    unordered_map<int, double> scores;

    for (const string& word : words) {
        auto it = comp.find(word);
        if (it == comp.end()) continue;

        int doc_freq = static_cast<int>(it->second.size());
        double idf = log2(static_cast<double>(total_docs) / doc_freq);

        for (const auto& [doc_id, bytes] : it->second) {
            scores[doc_id] += static_cast<double>(decompress(bytes).size()) * idf;
        }
    }

    vector<SearchResult> results;
    results.reserve(scores.size());
    for (const auto& [doc_id, score] : scores) {
        results.push_back({doc_id, score, {}});
    }

    sort(results.begin(), results.end(),
         [](const SearchResult& a, const SearchResult& b) { return a.score > b.score; });
    return results;
}

// --- binary index file read/write ---

void writeU32(std::ostream& os, uint32_t v) {
    char buf[4]={static_cast<char>(v&0xFF),static_cast<char>((v>>8)&0xFF),
                 static_cast<char>((v>>16)&0xFF),static_cast<char>((v>>24)&0xFF)};
    os.write(buf,4);
}
uint32_t readU32(std::istream& is) {
    unsigned char buf[4]; is.read(reinterpret_cast<char*>(buf),4);
    return static_cast<uint32_t>(buf[0])|(static_cast<uint32_t>(buf[1])<<8)
          |(static_cast<uint32_t>(buf[2])<<16)|(static_cast<uint32_t>(buf[3])<<24);
}
void writeStr(std::ostream& os,const string& s){
    writeU32(os,static_cast<uint32_t>(s.size()));
    os.write(s.data(),static_cast<std::streamsize>(s.size()));
}
string readStr(std::istream& is){
    uint32_t len=readU32(is); string s(len,'\0');
    is.read(s.data(),static_cast<std::streamsize>(len)); return s;
}
void writeBlob(std::ostream& os,const CompList& b){
    writeU32(os,static_cast<uint32_t>(b.size()));
    os.write(reinterpret_cast<const char*>(b.data()),static_cast<std::streamsize>(b.size()));
}
CompList readBlob(std::istream& is){
    uint32_t len=readU32(is); CompList b(len);
    is.read(reinterpret_cast<char*>(b.data()),static_cast<std::streamsize>(len)); return b;
}

static void writeCompIndex(std::ostream& os, const CompIndex& ci) {
    writeU32(os, static_cast<uint32_t>(ci.size()));
    for (const auto& [word, postings] : ci) {
        writeStr(os, word);
        writeU32(os, static_cast<uint32_t>(postings.size()));
        for (const auto& [doc_id, bytes] : postings) {
            writeU32(os, static_cast<uint32_t>(doc_id));
            writeBlob(os, bytes);
        }
    }
}

static void readCompIndex(std::istream& is, CompIndex& ci) {
    uint32_t num_terms = readU32(is);
    ci.reserve(num_terms);
    for (uint32_t i = 0; i < num_terms; ++i) {
        string word = readStr(is);
        uint32_t num_posts = readU32(is);
        CompPostings& posts = ci[word];
        posts.reserve(num_posts);
        for (uint32_t j = 0; j < num_posts; ++j) {
            int doc_id = static_cast<int>(readU32(is));
            posts[doc_id] = readBlob(is);
        }
    }
}

static const uint32_t MAGIC   = 0x48435253;   // "SRCH"
static const uint8_t  VERSION = 0x02;          // v2 adds line index

// writes the doc registry + both indexes to disk
void saveIndex(const string& filepath,
               const CompIndex& comp, const CompLineIndex& comp_lines,
               const DocRegistry& registry) {
    ofstream out(filepath, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write index: " + filepath);

    writeU32(out, MAGIC);
    out.put(static_cast<char>(VERSION));
    writeU32(out, static_cast<uint32_t>(registry.id_to_path.size()));

    for (const auto& [id, path] : registry.id_to_path) {
        writeU32(out, static_cast<uint32_t>(id));
        writeStr(out, path);
    }

    writeCompIndex(out, comp);
    writeCompIndex(out, comp_lines);

    if (!out) throw std::runtime_error("write error on: " + filepath);
    cerr << "saved -> " << filepath
         << " (" << fs::file_size(filepath) / 1024 << " KB)\n";
}

// loads the doc registry + both indexes back from disk
void loadIndex(const string& filepath,
               CompIndex& comp, CompLineIndex& comp_lines,
               DocRegistry& registry) {
    ifstream in(filepath, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open index: " + filepath);

    uint32_t magic = readU32(in);
    if (magic != MAGIC)
        throw std::runtime_error("bad magic bytes, not a valid index file");
    uint8_t ver = static_cast<uint8_t>(in.get());
    if (ver < 0x01 || ver > VERSION)
        throw std::runtime_error("unsupported index version: " + std::to_string(ver));

    uint32_t num_docs = readU32(in);
    registry.id_to_path.reserve(num_docs);
    for (uint32_t i = 0; i < num_docs; ++i) {
        int    id   = static_cast<int>(readU32(in));
        string path = readStr(in);
        registry.id_to_path[id] = path;
        registry.next_id = std::max(registry.next_id, id + 1);
    }
    if (!in) throw std::runtime_error("corrupted index: truncated at registry");

    readCompIndex(in, comp);

    if (ver >= 0x02) {
        readCompIndex(in, comp_lines);
    }
    if (!in && !in.eof())
        throw std::runtime_error("corrupted index: truncated during term read");
}

// escapes a string for embedding in JSON output
string jsonEscape(const string& s) {
    string out; out.reserve(s.size());
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out+="\\\""; break; case '\\': out+="\\\\"; break;
            case '\n': out+="\\n";  break; case '\r': out+="\\r";  break;
            case '\t': out+="\\t";  break;
            default:
                if (c<0x20||c>0x7E){char buf[8];snprintf(buf,sizeof(buf),"\\u%04x",c);out+=buf;}
                else out+=static_cast<char>(c);
        }
    }
    return out;
}

// splits a query string into normalized search terms
vector<string> parseQueryWords(const string& raw_query) {
    vector<string> words;
    istringstream iss(raw_query);
    string w;
    while (iss >> w) {
        string t = normalize(w);
        if (!t.empty()) words.push_back(t);
    }
    return words;
}

// looks up the line number a given word occurrence falls on
int findLineForWord(const CompIndex& comp, const CompLineIndex& comp_lines,
                     const string& word, int doc_id, int word_offset) {
    auto word_it = comp.find(word);
    auto line_it = comp_lines.find(word);
    if (word_it == comp.end() || line_it == comp_lines.end()) return -1;

    auto word_doc = word_it->second.find(doc_id);
    auto line_doc = line_it->second.find(doc_id);
    if (word_doc == word_it->second.end() || line_doc == line_it->second.end()) return -1;

    RawList offsets   = decompress(word_doc->second);
    RawList line_nums = decompress(line_doc->second);

    auto found = std::lower_bound(offsets.begin(), offsets.end(), word_offset);
    if (found == offsets.end() || *found != word_offset) return -1;

    size_t idx = static_cast<size_t>(found - offsets.begin());
    return (idx < line_nums.size()) ? line_nums[idx] : -1;
}

// keeps only phrase hits where every word in the phrase landed on the same line
void filterPhraseHitsToSameLine(vector<SearchResult>& phrase_hits, const vector<string>& words,
                                 const CompIndex& comp, const CompLineIndex& comp_lines) {
    for (auto& r : phrase_hits) {
        vector<int> same_line_positions;
        for (int pos : r.positions) {
            int start_line = findLineForWord(comp, comp_lines, words.front(), r.doc_id, pos);
            int end_line   = findLineForWord(comp, comp_lines, words.back(), r.doc_id,
                                              pos + static_cast<int>(words.size()) - 1);
            if (start_line > 0 && start_line == end_line) {
                same_line_positions.push_back(pos);
            }
        }
        r.positions = same_line_positions;
    }

    phrase_hits.erase(
        std::remove_if(phrase_hits.begin(), phrase_hits.end(),
            [](const SearchResult& r) { return r.positions.empty(); }),
        phrase_hits.end());
}

// drops ranked hits that don't actually contain every query word
void filterRankedHitsByMembership(vector<SearchResult>& ranked_hits, const vector<string>& words,
                                   const CompIndex& comp) {
    ranked_hits.erase(
        std::remove_if(ranked_hits.begin(), ranked_hits.end(),
            [&](const SearchResult& r) {
                for (const string& w : words) {
                    auto it = comp.find(w);
                    if (it == comp.end() || !it->second.count(r.doc_id)) return true;
                }
                return false;
            }),
        ranked_hits.end());
}

// writes the phrase_hits array of the JSON response
void writePhraseHitsJSON(const vector<SearchResult>& phrase_hits, const vector<string>& words,
                          const CompIndex& comp, const CompLineIndex& comp_lines,
                          const DocRegistry& registry) {
    cout << "  \"phrase_hits\": [\n";
    for (size_t i = 0; i < phrase_hits.size(); ++i) {
        const SearchResult& r = phrase_hits[i];
        const string& fp = registry.lookup(r.doc_id);

        unordered_set<int> req_lines;
        vector<int> target_lns;
        for (int pos : r.positions) {
            int ln = findLineForWord(comp, comp_lines, words[0], r.doc_id, pos);
            if (ln > 0) req_lines.insert(ln);
            target_lns.push_back(ln);
        }
        auto line_cache = getLinesSinglePass(fp, req_lines);

        cout << "    {\n"
             << "      \"doc_id\": "     << r.doc_id << ",\n"
             << "      \"filename\": \"" << jsonEscape(fs::path(fp).filename().string()) << "\",\n"
             << "      \"hit_count\": "  << r.positions.size() << ",\n"
             << "      \"hits\": [\n";

        for (size_t j = 0; j < target_lns.size(); ++j) {
            int ln = target_lns[j];
            string full_line = (ln > 0 && line_cache.count(ln)) ? line_cache[ln] : "";
            cout << "        { \"line_number\": " << std::max(ln, 0)
                 << ", \"full_line\": \"" << jsonEscape(full_line) << "\" }";
            if (j + 1 < target_lns.size()) cout << ",";
            cout << "\n";
        }
        cout << "      ]\n    }";
        if (i + 1 < phrase_hits.size()) cout << ",";
        cout << "\n";
    }
    cout << "  ],\n";
}

// writes the ranked_hits array of the JSON response
void writeRankedHitsJSON(const vector<SearchResult>& ranked_hits, const vector<string>& words,
                          const CompLineIndex& comp_lines, const DocRegistry& registry) {
    cout << "  \"ranked_hits\": [\n";
    for (size_t i = 0; i < ranked_hits.size(); ++i) {
        const SearchResult& r = ranked_hits[i];
        const string& fp = registry.lookup(r.doc_id);

        unordered_set<int> req_lines;
        for (const string& word : words) {
            auto line_it = comp_lines.find(word);
            if (line_it == comp_lines.end()) continue;
            auto line_doc = line_it->second.find(r.doc_id);
            if (line_doc == line_it->second.end()) continue;
            for (int ln : decompress(line_doc->second)) req_lines.insert(ln);
        }
        auto line_cache = getLinesSinglePass(fp, req_lines);

        vector<int> sorted_lns(req_lines.begin(), req_lines.end());
        sort(sorted_lns.begin(), sorted_lns.end());

        cout << "    {\n"
             << "      \"doc_id\": "     << r.doc_id << ",\n"
             << "      \"filename\": \"" << jsonEscape(fs::path(fp).filename().string()) << "\",\n"
             << "      \"score\": "
             << std::fixed << std::setprecision(4) << r.score << ",\n"
             << "      \"occurrences\": [\n";

        for (size_t j = 0; j < sorted_lns.size(); ++j) {
            int ln = sorted_lns[j];
            cout << "        { \"line_number\": " << ln
                 << ", \"full_line\": \"" << jsonEscape(line_cache[ln]) << "\" }";
            if (j + 1 < sorted_lns.size()) cout << ",";
            cout << "\n";
        }
        cout << "      ]\n    }";
        if (i + 1 < ranked_hits.size()) cout << ",";
        cout << "\n";
    }
    cout << "  ]\n}\n";
}

// runs a query and writes the full result as JSON to stdout
void searchToJSON(const string& raw_query,
                  const CompIndex& comp,
                  const CompLineIndex& comp_lines,
                  const DocRegistry& registry) {
    vector<string> words = parseQueryWords(raw_query);
    int total_docs = registry.size();

    vector<SearchResult> phrase_hits;
    if (words.size() > 1) phrase_hits = phraseMatch(words, comp);
    vector<SearchResult> ranked_hits = tfidfRank(words, comp, total_docs);

    if (words.size() > 1) {
        filterPhraseHitsToSameLine(phrase_hits, words, comp, comp_lines);
        filterRankedHitsByMembership(ranked_hits, words, comp);
    }

    cout << "{\n";
    cout << "  \"query\": \""    << jsonEscape(raw_query) << "\",\n";
    cout << "  \"total_docs\": " << total_docs             << ",\n";

    writePhraseHitsJSON(phrase_hits, words, comp, comp_lines, registry);
    writeRankedHitsJSON(ranked_hits, words, comp_lines, registry);
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        cerr << "usage:\n"
             << "  ./indexer build  <data_dir>  <index_file>\n"
             << "  ./indexer search <index_file> <query words...>\n";
        return 1;
    }

    string mode = argv[1];

    if (mode == "build") {
        if (argc < 4) { cerr << "usage: ./indexer build <data_dir> <index_file>\n"; return 1; }

        RawIndex    raw_index;
        RawLineIndex raw_lines;
        DocRegistry registry;

        cerr << "building index...\n";
        try { indexDirectoryConcurrent(argv[2], raw_index, raw_lines, registry); }
        catch (const std::exception& e) { cerr << "error: " << e.what() << "\n"; return 1; }

        printStats(raw_index, registry);

        CompIndex    comp_index = compressIndex(raw_index);
        CompLineIndex comp_lines = compressIndex(raw_lines);
        printMemoryStats(raw_index, comp_index, raw_lines, comp_lines);

        try { saveIndex(argv[3], comp_index, comp_lines, registry); }
        catch (const std::exception& e) { cerr << "error: " << e.what() << "\n"; return 1; }

        cerr << "build complete\n";
        return 0;
    }

    if (mode == "search") {
        if (argc < 4) { cerr << "usage: ./indexer search <index_file> <query...>\n"; return 1; }

        string query = argv[3];
        for (int i = 4; i < argc; ++i) { query += " "; query += argv[i]; }

        CompIndex    comp_index;
        CompLineIndex comp_lines;
        DocRegistry  registry;

        try { loadIndex(argv[2], comp_index, comp_lines, registry); }
        catch (const std::exception& e) {
            cout << "{\"error\": \"" << jsonEscape(e.what()) << "\"}\n";
            return 1;
        }

        searchToJSON(query, comp_index, comp_lines, registry);
        return 0;
    }

    cerr << "unknown mode: " << mode << ", use 'build' or 'search'\n";
    return 1;
}
