#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cstring>
#include <sys/stat.h>

using namespace std;

const string DATA_FILE = "bpt_data.bin";
const string META_FILE = "bpt_meta.bin";

const int MAX_KEY_LEN = 64;
const int ORDER = 64;

struct Meta {
    int64_t root_offset;
    int64_t next_offset;
    int node_count;
};

struct Key {
    char data[MAX_KEY_LEN];
    int len;

    Key() : len(0) { memset(data, 0, MAX_KEY_LEN); }
    Key(const string& s) : len(s.length()) {
        memset(data, 0, MAX_KEY_LEN);
        memcpy(data, s.c_str(), min((int)s.length(), MAX_KEY_LEN));
    }

    bool operator<(const Key& other) const {
        int min_len = min(len, other.len);
        int cmp = memcmp(data, other.data, min_len);
        if (cmp != 0) return cmp < 0;
        return len < other.len;
    }

    bool operator<=(const Key& other) const {
        return !(other < *this);
    }

    bool operator>=(const Key& other) const {
        return !(*this < other);
    }

    bool operator==(const Key& other) const {
        if (len != other.len) return false;
        return memcmp(data, other.data, len) == 0;
    }

    bool operator!=(const Key& other) const {
        return !(*this == other);
    }
};

struct ValueList {
    int values[ORDER];
    int count;

    ValueList() : count(0) {
        memset(values, 0, sizeof(values));
    }

    bool insert(int val) {
        int lo = 0, hi = count;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (values[mid] < val) lo = mid + 1;
            else hi = mid;
        }
        if (lo < count && values[lo] == val) return false;
        for (int i = count; i > lo; i--) values[i] = values[i-1];
        values[lo] = val;
        count++;
        return true;
    }

    bool remove(int val) {
        int lo = 0, hi = count;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (values[mid] < val) lo = mid + 1;
            else hi = mid;
        }
        if (lo >= count || values[lo] != val) return false;
        for (int i = lo; i < count - 1; i++) values[i] = values[i+1];
        count--;
        return true;
    }
};

enum NodeType { INTERNAL, LEAF };

struct Node {
    NodeType type;
    int key_count;
    Key keys[ORDER];
    int64_t children[ORDER + 1];
    ValueList values[ORDER];

    Node() : type(LEAF), key_count(0) {
        memset(children, 0, sizeof(children));
        for (int i = 0; i < ORDER; i++) {
            values[i] = ValueList();
        }
    }

    bool isLeaf() const { return type == LEAF; }
    bool isFull() const { return key_count >= ORDER; }
};

class BPTree {
private:
    Meta meta;
    string data_filename;
    string meta_filename;

    void init_files() {
        meta.root_offset = -1;
        meta.next_offset = 0;
        meta.node_count = 0;

        ofstream meta_file(meta_filename, ios::binary);
        meta_file.write((char*)&meta, sizeof(Meta));
        meta_file.close();

        ofstream data_file(data_filename, ios::binary);
        data_file.close();
    }

    void load_meta() {
        ifstream meta_file(meta_filename, ios::binary);
        meta_file.read((char*)&meta, sizeof(Meta));
        meta_file.close();
    }

    void save_meta() {
        ofstream meta_file(meta_filename, ios::binary);
        meta_file.write((char*)&meta, sizeof(Meta));
        meta_file.close();
    }

    int64_t alloc_node() {
        int64_t offset = meta.next_offset;
        meta.next_offset += sizeof(Node);
        meta.node_count++;
        save_meta();
        return offset;
    }

    void write_node(int64_t offset, const Node& node) {
        fstream file(data_filename, ios::binary | ios::in | ios::out);
        file.seekp(offset);
        file.write((char*)&node, sizeof(Node));
        file.close();
    }

    Node read_node(int64_t offset) {
        Node node;
        ifstream file(data_filename, ios::binary);
        file.seekg(offset);
        file.read((char*)&node, sizeof(Node));
        file.close();
        return node;
    }

    int64_t create_leaf() {
        int64_t offset = alloc_node();
        Node node;
        node.type = LEAF;
        write_node(offset, node);
        return offset;
    }

    int64_t create_internal() {
        int64_t offset = alloc_node();
        Node node;
        node.type = INTERNAL;
        write_node(offset, node);
        return offset;
    }

    pair<int64_t, Key> split_leaf(int64_t offset) {
        Node node = read_node(offset);
        int64_t new_offset = create_leaf();
        Node new_node;

        int mid = (node.key_count + 1) / 2;
        Key split_key = node.keys[mid];

        for (int i = mid; i < node.key_count; i++) {
            new_node.keys[i - mid] = node.keys[i];
            new_node.values[i - mid] = node.values[i];
        }
        new_node.key_count = node.key_count - mid;
        new_node.children[0] = node.children[0];
        node.key_count = mid;

        new_node.children[0] = node.children[0];
        node.children[0] = new_offset;

        write_node(offset, node);
        write_node(new_offset, new_node);

        return {new_offset, split_key};
    }

    pair<int64_t, Key> split_internal(int64_t offset) {
        Node node = read_node(offset);
        int64_t new_offset = create_internal();
        Node new_node;

        int mid = node.key_count / 2;
        Key split_key = node.keys[mid];

        new_node.key_count = node.key_count - mid - 1;
        for (int i = mid + 1; i < node.key_count; i++) {
            new_node.keys[i - mid - 1] = node.keys[i];
            new_node.children[i - mid - 1] = node.children[i];
        }
        new_node.children[new_node.key_count] = node.children[node.key_count];

        node.key_count = mid;

        write_node(offset, node);
        write_node(new_offset, new_node);

        return {new_offset, split_key};
    }

    void insert_into_internal(Node& node, const Key& key, int64_t right_child) {
        int i = node.key_count - 1;
        while (i >= 0 && key < node.keys[i]) {
            node.keys[i + 1] = node.keys[i];
            node.children[i + 2] = node.children[i + 1];
            i--;
        }
        node.keys[i + 1] = key;
        node.children[i + 2] = right_child;
        node.key_count++;
    }

    void insert_into_leaf(Node& node, const Key& key, int value) {
        int i = node.key_count - 1;
        while (i >= 0 && key < node.keys[i]) {
            node.keys[i + 1] = node.keys[i];
            node.values[i + 1] = node.values[i];
            i--;
        }
        node.keys[i + 1] = key;
        node.values[i + 1] = ValueList();
        node.values[i + 1].insert(value);
        node.key_count++;
    }

    int find_child_index(const Node& node, const Key& key) {
        int idx = 0;
        while (idx < node.key_count && key >= node.keys[idx]) {
            idx++;
        }
        return idx;
    }

public:
    BPTree(const string& data_fn = DATA_FILE, const string& meta_fn = META_FILE)
        : data_filename(data_fn), meta_filename(meta_fn) {
        struct stat statbuf;
        bool data_exists = (stat(data_filename.c_str(), &statbuf) == 0);
        bool meta_exists = (stat(meta_filename.c_str(), &statbuf) == 0);

        if (!data_exists || !meta_exists) {
            init_files();
        } else {
            load_meta();
        }
    }

    bool file_exists(const string& filename) {
        struct stat buffer;
        return (stat(filename.c_str(), &buffer) == 0);
    }

    void insert(const string& key_str, int value) {
        Key key(key_str);

        if (meta.root_offset == -1) {
            meta.root_offset = create_leaf();
            save_meta();
        }

        vector<int64_t> path;
        vector<int> indices;

        int64_t current = meta.root_offset;
        while (true) {
            path.push_back(current);
            Node node = read_node(current);

            if (node.isLeaf()) {
                for (int i = 0; i < node.key_count; i++) {
                    if (node.keys[i] == key) {
                        if (node.values[i].insert(value)) {
                            write_node(current, node);
                        }
                        return;
                    }
                }

                if (node.isFull()) {
                    auto [new_offset, split_key] = split_leaf(current);
                    node = read_node(current);

                    if (key < split_key) {
                        insert_into_leaf(node, key, value);
                        write_node(current, node);
                    } else {
                        Node new_node = read_node(new_offset);
                        insert_into_leaf(new_node, key, value);
                        write_node(new_offset, new_node);
                    }

                    insert_split_key(path, indices, current, new_offset, split_key);
                } else {
                    insert_into_leaf(node, key, value);
                    write_node(current, node);
                }
                break;
            } else {
                int idx = find_child_index(node, key);
                indices.push_back(idx);
                current = node.children[idx];
            }
        }
    }

    void insert_split_key(const vector<int64_t>& path, const vector<int>& indices,
                          int64_t left, int64_t right, const Key& key) {
        if (path.size() == 1) {
            int64_t new_root = create_internal();
            Node root = read_node(new_root);
            root.key_count = 1;
            root.keys[0] = key;
            root.children[0] = left;
            root.children[1] = right;
            write_node(new_root, root);
            meta.root_offset = new_root;
            save_meta();
            return;
        }

        int64_t parent_offset = path[path.size() - 2];
        Node parent = read_node(parent_offset);

        if (parent.isFull()) {
            auto [new_parent_offset, split_key] = split_internal(parent_offset);
            parent = read_node(parent_offset);

            int parent_idx = indices.empty() ? 0 : indices[indices.size() - 1];
            if (parent_idx < parent.key_count) {
                insert_into_internal(parent, key, right);
                write_node(parent_offset, parent);
            } else {
                Node new_parent = read_node(new_parent_offset);
                insert_into_internal(new_parent, key, right);
                write_node(new_parent_offset, new_parent);
            }

            insert_split_key(vector<int64_t>(path.begin(), path.end() - 1),
                             vector<int>(indices.begin(), indices.empty() ? indices.end() : indices.end() - 1),
                             parent_offset, new_parent_offset, split_key);
        } else {
            insert_into_internal(parent, key, right);
            write_node(parent_offset, parent);
        }
    }

    void remove(const string& key_str, int value) {
        if (meta.root_offset == -1) return;
        Key key(key_str);

        int64_t current = meta.root_offset;
        while (true) {
            Node node = read_node(current);

            if (node.isLeaf()) {
                for (int i = 0; i < node.key_count; i++) {
                    if (node.keys[i] == key) {
                        if (node.values[i].remove(value)) {
                            if (node.values[i].count == 0) {
                                for (int j = i; j < node.key_count - 1; j++) {
                                    node.keys[j] = node.keys[j + 1];
                                    node.values[j] = node.values[j + 1];
                                }
                                node.key_count--;
                            }
                            write_node(current, node);
                        }
                        return;
                    }
                }
                return;
            } else {
                int idx = find_child_index(node, key);
                current = node.children[idx];
            }
        }
    }

    vector<int> find(const string& key_str) {
        vector<int> result;
        if (meta.root_offset == -1) return result;
        Key key(key_str);

        int64_t current = meta.root_offset;
        while (true) {
            Node node = read_node(current);

            if (node.isLeaf()) {
                for (int i = 0; i < node.key_count; i++) {
                    if (node.keys[i] == key) {
                        for (int j = 0; j < node.values[i].count; j++) {
                            result.push_back(node.values[i].values[j]);
                        }
                        return result;
                    }
                }
                return result;
            } else {
                int idx = find_child_index(node, key);
                current = node.children[idx];
            }
        }
    }
};

int main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    BPTree bpt;

    int n;
    cin >> n;

    for (int i = 0; i < n; i++) {
        string cmd;
        cin >> cmd;

        if (cmd == "insert") {
            string index;
            int value;
            cin >> index >> value;
            bpt.insert(index, value);
        } else if (cmd == "delete") {
            string index;
            int value;
            cin >> index >> value;
            bpt.remove(index, value);
        } else if (cmd == "find") {
            string index;
            cin >> index;
            vector<int> values = bpt.find(index);
            if (values.empty()) {
                cout << "null" << "\n";
            } else {
                for (size_t j = 0; j < values.size(); j++) {
                    if (j > 0) cout << " ";
                    cout << values[j];
                }
                cout << "\n";
            }
        }
    }

    return 0;
}
