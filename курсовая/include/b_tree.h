#ifndef SYS_PROG_B_TREE_H
#define SYS_PROG_B_TREE_H

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <stack>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>
#include <boost/container/static_vector.hpp>

#include <pp_allocator.h>
#include <associative_container.h>

/*
 * B-tree with minimum degree t.
 *
 * Important implementation detail:
 * btree_node does not contain a parent pointer.  Iterator paths contain
 * pointers to the links which lead to nodes, and the insertion/deletion
 * algorithms pass the parent explicitly through recursion.
 */
template <
    typename tkey,
    typename tvalue,
    comparator<tkey> compare = std::less<tkey>,
    std::size_t t = 5>
class B_tree final : private compare
{
    static_assert(t >= 2, "B-tree minimum degree must be at least 2");

public:
    using tree_data_type = std::pair<tkey, tvalue>;
    using tree_data_type_const = std::pair<const tkey, tvalue>;
    using value_type = tree_data_type_const;

    class btree_exception : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    class key_not_found : public std::out_of_range
    {
    public:
        using std::out_of_range::out_of_range;
    };

    class invalid_iterator : public btree_exception
    {
    public:
        using btree_exception::btree_exception;
    };

private:
    static constexpr std::size_t minimum_keys_in_node = t - 1;
    static constexpr std::size_t maximum_keys_in_node = 2 * t - 1;

    struct btree_node
    {
        boost::container::static_vector<tree_data_type, maximum_keys_in_node + 1> _keys;
        boost::container::static_vector<btree_node*, maximum_keys_in_node + 2> _pointers;

        btree_node() noexcept = default;
    };

    pp_allocator<value_type> _allocator;
    btree_node* _root = nullptr;
    std::size_t _size = 0;

    pp_allocator<value_type> get_allocator() const noexcept
    {
        return _allocator;
    }

    bool compare_keys(const tkey& lhs, const tkey& rhs) const
    {
        return compare::operator()(lhs, rhs);
    }

    bool equivalent_keys(const tkey& lhs, const tkey& rhs) const
    {
        return !compare_keys(lhs, rhs) && !compare_keys(rhs, lhs);
    }

    static std::size_t lower_index(const btree_node* node, const tkey& key, const compare& cmp)
    {
        std::size_t left = 0;
        std::size_t right = node->_keys.size();
        while (left < right)
        {
            const std::size_t middle = left + (right - left) / 2;
            if (cmp(node->_keys[middle].first, key))
                left = middle + 1;
            else
                right = middle;
        }
        return left;
    }

    std::size_t lower_index(const btree_node* node, const tkey& key) const
    {
        return lower_index(node, key, static_cast<const compare&>(*this));
    }

    static std::size_t upper_index(const btree_node* node, const tkey& key, const compare& cmp)
    {
        std::size_t left = 0;
        std::size_t right = node->_keys.size();
        while (left < right)
        {
            const std::size_t middle = left + (right - left) / 2;
            if (!cmp(key, node->_keys[middle].first))
                left = middle + 1;
            else
                right = middle;
        }
        return left;
    }

    std::size_t upper_index(const btree_node* node, const tkey& key) const
    {
        return upper_index(node, key, static_cast<const compare&>(*this));
    }

    btree_node* create_node()
    {
        return _allocator.template new_object<btree_node>();
    }

    void destroy_node(btree_node* node) noexcept
    {
        if (node != nullptr)
            _allocator.delete_object(node);
    }

    void destroy_subtree(btree_node* node) noexcept
    {
        if (node == nullptr)
            return;

        for (btree_node* child : node->_pointers)
            destroy_subtree(child);

        destroy_node(node);
    }

    btree_node* clone_subtree(const btree_node* source)
    {
        if (source == nullptr)
            return nullptr;

        btree_node* result = create_node();
        try
        {
            for (const tree_data_type& item : source->_keys)
                result->_keys.emplace_back(item);

            for (const btree_node* child : source->_pointers)
                result->_pointers.push_back(clone_subtree(child));
        }
        catch (...)
        {
            destroy_subtree(result);
            throw;
        }
        return result;
    }

    void split_child_allocated(btree_node* parent, std::size_t child_index)
    {
        btree_node* child = parent->_pointers[child_index];
        btree_node* right = create_node();

        try
        {
            tree_data_type promoted = child->_keys[t];

            for (std::size_t i = t + 1; i < child->_keys.size(); ++i)
                right->_keys.push_back(std::move(child->_keys[i]));

            if (!child->_pointers.empty())
            {
                for (std::size_t i = t + 1; i < child->_pointers.size(); ++i)
                    right->_pointers.push_back(child->_pointers[i]);
                child->_pointers.erase(child->_pointers.begin() + static_cast<std::ptrdiff_t>(t + 1), child->_pointers.end());
            }

            child->_keys.erase(child->_keys.begin() + static_cast<std::ptrdiff_t>(t), child->_keys.end());

            parent->_keys.insert(
                parent->_keys.begin() + static_cast<std::ptrdiff_t>(child_index),
                std::move(promoted));
            parent->_pointers.insert(
                parent->_pointers.begin() + static_cast<std::ptrdiff_t>(child_index + 1),
                right);
        }
        catch (...)
        {
            destroy_subtree(right);
            throw;
        }
    }

    void insert_recursive(btree_node* node, tree_data_type data)
    {
        const std::size_t position = lower_index(node, data.first);

        if (node->_pointers.empty())
        {
            node->_keys.insert(
                node->_keys.begin() + static_cast<std::ptrdiff_t>(position),
                std::move(data));
            return;
        }

        btree_node* child = node->_pointers[position];
        insert_recursive(child, std::move(data));

        if (child->_keys.size() == maximum_keys_in_node + 1)
            split_child_allocated(node, position);
    }

    void insert_new(tree_data_type data)
    {
        if (_root == nullptr)
        {
            _root = create_node();
            try
            {
                _root->_keys.push_back(std::move(data));
            }
            catch (...)
            {
                destroy_node(_root);
                _root = nullptr;
                throw;
            }
            ++_size;
            return;
        }

        insert_recursive(_root, std::move(data));

        if (_root->_keys.size() == maximum_keys_in_node + 1)
        {
            btree_node* new_root = create_node();
            try
            {
                new_root->_pointers.push_back(_root);
                split_child_allocated(new_root, 0);
            }
            catch (...)
            {
                // new_root owns no newly created child if split failed before
                // insertion into its pointer vector; _root remains the tree.
                new_root->_pointers.clear();
                destroy_node(new_root);
                throw;
            }
            _root = new_root;
        }

        ++_size;
    }

    bool find_path(const tkey& key, std::vector<std::pair<btree_node**, std::size_t>>& path) const
    {
        path.clear();
        btree_node** link = const_cast<btree_node**>(&_root);
        btree_node* node = *link;
        std::size_t edge = 0;

        while (node != nullptr)
        {
            const std::size_t index = lower_index(node, key);
            path.emplace_back(link, edge);

            if (index < node->_keys.size() && equivalent_keys(node->_keys[index].first, key))
            {
                return true;
            }

            if (node->_pointers.empty())
            {
                path.pop_back();
                return false;
            }

            edge = index;
            link = &node->_pointers[index];
            node = *link;
        }
        path.clear();
        return false;
    }

    btree_node** child_link(btree_node* node, std::size_t index)
    {
        return &node->_pointers[index];
    }

    static void vector_to_stack(
        const std::vector<std::pair<btree_node**, std::size_t>>& path,
        std::stack<std::pair<btree_node**, std::size_t>>& result)
    {
        result = {};
        for (const auto& item : path)
            result.push(item);
    }

    bool make_bound_path(
    const tkey& key,
    bool upper,
    std::vector<std::pair<btree_node**, std::size_t>>& result,
    std::size_t& result_index) const
    {
        result.clear();
        result_index = 0;

        std::vector<std::pair<btree_node**, std::size_t>> path;
        std::vector<std::pair<btree_node**, std::size_t>> candidate_path;
        std::size_t candidate_index = 0;
        bool found = false;

        btree_node** link = const_cast<btree_node**>(&_root);
        btree_node* node = *link;
        std::size_t edge = 0;

        while (node != nullptr)
        {
            path.emplace_back(link, edge);

            const std::size_t i =
                upper ? upper_index(node, key)
                    : lower_index(node, key);

            if (i < node->_keys.size())
            {
                candidate_path = path;
                candidate_index = i;
                found = true;
            }

            if (node->_pointers.empty())
            {
                break;
            }

            edge = i;
            link = &node->_pointers[edge];
            node = *link;
        }

        if (!found)
        {
            return false;
        }

        result = std::move(candidate_path);
        result_index = candidate_index;
        return true;
    }

    btree_node** rightmost_link(std::stack<std::pair<btree_node**, std::size_t>>& path)
    {
        if (_root == nullptr)
            return nullptr;

        path = {};
        btree_node** link = &_root;
        btree_node* node = _root;
        path.emplace(link, 0);

        while (!node->_pointers.empty())
        {
            const std::size_t edge = node->_pointers.size() - 1;
            link = &node->_pointers[edge];
            node = *link;
            path.emplace(link, edge);
        }

        return link;
    }

    void delete_key_from_leaf(btree_node* node, std::size_t index)
    {
        node->_keys.erase(node->_keys.begin() + static_cast<std::ptrdiff_t>(index));
    }

    void borrow_from_left(btree_node* parent, std::size_t child_index)
    {
        btree_node* child = parent->_pointers[child_index];
        btree_node* left = parent->_pointers[child_index - 1];

        child->_keys.insert(child->_keys.begin(), parent->_keys[child_index - 1]);
        parent->_keys[child_index - 1] = left->_keys.back();
        left->_keys.pop_back();

        if (!left->_pointers.empty())
        {
            child->_pointers.insert(child->_pointers.begin(), left->_pointers.back());
            left->_pointers.pop_back();
        }
    }

    void borrow_from_right(btree_node* parent, std::size_t child_index)
    {
        btree_node* child = parent->_pointers[child_index];
        btree_node* right = parent->_pointers[child_index + 1];

        child->_keys.push_back(parent->_keys[child_index]);
        parent->_keys[child_index] = right->_keys.front();
        right->_keys.erase(right->_keys.begin());

        if (!right->_pointers.empty())
        {
            child->_pointers.push_back(right->_pointers.front());
            right->_pointers.erase(right->_pointers.begin());
        }
    }

    btree_node* merge_children(btree_node* parent, std::size_t left_index)
    {
        btree_node* left = parent->_pointers[left_index];
        btree_node* right = parent->_pointers[left_index + 1];

        left->_keys.push_back(std::move(parent->_keys[left_index]));
        for (auto& item : right->_keys)
            left->_keys.push_back(std::move(item));

        for (btree_node* child : right->_pointers)
            left->_pointers.push_back(child);

        parent->_keys.erase(parent->_keys.begin() + static_cast<std::ptrdiff_t>(left_index));
        parent->_pointers.erase(parent->_pointers.begin() + static_cast<std::ptrdiff_t>(left_index + 1));

        right->_pointers.clear();
        right->_keys.clear();
        destroy_node(right);
        return left;
    }

    void erase_recursive(btree_node* node, const tkey& key)
    {
        std::size_t index = lower_index(node, key);

        if (index < node->_keys.size() && equivalent_keys(node->_keys[index].first, key))
        {
            if (node->_pointers.empty())
            {
                delete_key_from_leaf(node, index);
                return;
            }

            btree_node* left = node->_pointers[index];
            btree_node* right = node->_pointers[index + 1];

            if (left->_keys.size() >= t)
            {
                btree_node* cursor = left;
                while (!cursor->_pointers.empty())
                    cursor = cursor->_pointers.back();
                tree_data_type replacement = cursor->_keys.back();
                node->_keys[index] = replacement;
                erase_recursive(left, replacement.first);
                return;
            }

            if (right->_keys.size() >= t)
            {
                btree_node* cursor = right;
                while (!cursor->_pointers.empty())
                    cursor = cursor->_pointers.front();
                tree_data_type replacement = cursor->_keys.front();
                node->_keys[index] = replacement;
                erase_recursive(right, replacement.first);
                return;
            }

            btree_node* merged = merge_children(node, index);
            erase_recursive(merged, key);
            return;
        }

        if (node->_pointers.empty())
            return;

        btree_node* child = node->_pointers[index];

        if (child->_keys.size() == minimum_keys_in_node)
        {
            if (index > 0 && node->_pointers[index - 1]->_keys.size() >= t)
            {
                borrow_from_left(node, index);
            }
            else if (index + 1 < node->_pointers.size() && node->_pointers[index + 1]->_keys.size() >= t)
            {
                borrow_from_right(node, index);
            }
            else
            {
                if (index + 1 < node->_pointers.size())
                    child = merge_children(node, index);
                else
                    child = merge_children(node, index - 1);
            }
        }

        erase_recursive(child, key);
    }

    void erase_existing(const tkey& key)
    {
        if (_root == nullptr)
            return;

        erase_recursive(_root, key);
        --_size;

        if (_root->_keys.empty() && !_root->_pointers.empty())
        {
            btree_node* old_root = _root;
            _root = _root->_pointers.front();
            old_root->_pointers.clear();
            destroy_node(old_root);
        }
        else if (_root->_keys.empty() && _root->_pointers.empty())
        {
            destroy_node(_root);
            _root = nullptr;
        }
    }

public:
    class btree_iterator;
    class btree_reverse_iterator;   
    class btree_const_iterator;
    class btree_const_reverse_iterator;

    explicit B_tree(const compare& cmp = compare(), pp_allocator<value_type> alloc = pp_allocator<value_type>())
        : compare(cmp), _allocator(std::move(alloc))
    {
    }

    explicit B_tree(pp_allocator<value_type> alloc, const compare& comp = compare())
        : compare(comp), _allocator(std::move(alloc))
    {
    }

    template<input_iterator_for_pair<tkey, tvalue> iterator>
    explicit B_tree(
        iterator first,
        iterator last,
        const compare& cmp = compare(),
        pp_allocator<value_type> alloc = pp_allocator<value_type>())
        : compare(cmp), _allocator(std::move(alloc))
    {
        try
        {
            for (; first != last; ++first)
                emplace(first->first, first->second);
        }
        catch (...)
        {
            clear();
            throw;
        }
    }

    B_tree(
        std::initializer_list<std::pair<tkey, tvalue>> data,
        const compare& cmp = compare(),
        pp_allocator<value_type> alloc = pp_allocator<value_type>())
        : compare(cmp), _allocator(std::move(alloc))
    {
        try
        {
            for (const auto& item : data)
                emplace(item.first, item.second);
        }
        catch (...)
        {
            clear();
            throw;
        }
    }

    B_tree(const B_tree& other)
        : compare(static_cast<const compare&>(other)),
          _allocator(other._allocator),
          _root(nullptr),
          _size(0)
    {
        _root = clone_subtree(other._root);
        _size = other._size;
    }

    B_tree(B_tree&& other) noexcept
        : compare(std::move(static_cast<compare&>(other))),
          _allocator(std::move(other._allocator)),
          _root(other._root),
          _size(other._size)
    {
        other._root = nullptr;
        other._size = 0;
    }

    B_tree& operator=(const B_tree& other)
    {
        if (this == &other)
            return *this;

        B_tree temporary(other);
        std::swap(_root, temporary._root);
        std::swap(_size, temporary._size);
        _allocator = temporary._allocator;
        compare::operator=(static_cast<const compare&>(other));
        return *this;
    }

    B_tree& operator=(B_tree&& other) noexcept
    {
        if (this == &other)
            return *this;

        clear();
        _allocator = std::move(other._allocator);
        compare::operator=(std::move(static_cast<compare&>(other)));
        _root = other._root;
        _size = other._size;
        other._root = nullptr;
        other._size = 0;
        return *this;
    }

    ~B_tree() noexcept
    {
        clear();
    }

    class btree_iterator final
    {
        std::stack<std::pair<btree_node**, std::size_t>> _path;
        std::size_t _index = 0;

        friend class B_tree;
        friend class btree_reverse_iterator;
        friend class btree_const_iterator;
        friend class btree_const_reverse_iterator;

        static bool same_path(
            std::stack<std::pair<btree_node**, std::size_t>> lhs,
            std::stack<std::pair<btree_node**, std::size_t>> rhs)
        {
            if (lhs.size() != rhs.size())
                return false;
            while (!lhs.empty())
            {
                if (lhs.top() != rhs.top())
                    return false;
                lhs.pop();
                rhs.pop();
            }
            return true;
        }

    public:
        using value_type = tree_data_type_const;
        using reference = value_type&;
        using pointer = value_type*;
        using iterator_category = std::bidirectional_iterator_tag;
        using difference_type = std::ptrdiff_t;
        using self = btree_iterator;

        reference operator*() const noexcept
        {
            return reinterpret_cast<reference>((*_path.top().first)->_keys[_index]);
        }

        pointer operator->() const noexcept
        {
            return std::addressof(operator*());
        }

        self& operator++()
        {
            if (_path.empty())
                return *this;
            const auto original_path = _path;
            btree_node* node = *_path.top().first;
            if (node == nullptr)
                return *this;
            if (!node->_pointers.empty())
            {
                const std::size_t edge = _index + 1;

                if (edge < node->_pointers.size())
                {
                    auto link = &node->_pointers[edge];
                    _path.emplace(link, edge);
                    node = *link;

                    while (!node->_pointers.empty())
                    {
                        link = &node->_pointers[0];
                        node = *link;
                        _path.emplace(link, 0);
                    }

                    _index = 0;
                    return *this;
                }
            }

            if (_index + 1 < node->_keys.size())
            {
                ++_index;
                return *this;
            }
            std::size_t edge = _path.top().second;
            _path.pop();
            while (!_path.empty())
            {
                btree_node* parent = *_path.top().first;
                if (edge < parent->_keys.size())
                {
                    _index = edge;
                    return *this;
                }
                edge = _path.top().second;
                _path.pop();
            }
            _path = original_path;
            _index = node->_keys.size();
            return *this;
        }

        self operator++(int)
        {
            self result(*this);
            ++(*this);
            return result;
        }

        self& operator--()
        {
            if (_path.empty())
                return *this;
            btree_node* node = *_path.top().first;
            if (node == nullptr)
                return *this;
            if (_index == node->_keys.size())
            {
                if (!node->_keys.empty())
                    --_index;
                return *this;
            }
            if (_index > 0)
            {
                --_index;
                return *this;
            }
            if (!node->_pointers.empty() && _index < node->_pointers.size())
            {
                const std::size_t edge = _index;
                btree_node** link = &node->_pointers[edge];
                _path.emplace(link, edge);
                node = *link;
                while (!node->_pointers.empty())
                {
                    const std::size_t right = node->_pointers.size() - 1;
                    link = &node->_pointers[right];
                    node = *link;
                    _path.emplace(link, right);
                }
                _index = node->_keys.size() - 1;
                return *this;
            }
            std::size_t edge = _path.top().second;
            _path.pop();
            while (!_path.empty())
            {
                if (edge > 0)
                {
                    _index = edge - 1;
                    return *this;
                }
                edge = _path.top().second;
                _path.pop();
            }
            _index = 0;
            return *this;
        }

        self operator--(int)
        {
            self result(*this);
            --(*this);
            return result;
        }

        bool operator==(const self& other) const noexcept
        {
            return _index == other._index && same_path(_path, other._path);
        }

        bool operator!=(const self& other) const noexcept
        {
            return !(*this == other);
        }

        std::size_t depth() const noexcept
        {
            return _path.empty() ? 0 : _path.size() - 1;
        }

        std::size_t current_node_keys_count() const noexcept
        {
            return _path.empty() ? 0 : (*_path.top().first)->_keys.size();
        }

        bool is_terminate_node() const noexcept
        {
            return !_path.empty() && (*_path.top().first)->_pointers.empty();
        }

        std::size_t index() const noexcept
        {
            return _index;
        }

        explicit btree_iterator(const std::stack<std::pair<btree_node**, std::size_t>>& path = {}, std::size_t index = 0) : _path(path), _index(index)
        {
        }
    };

    class btree_const_iterator final
    {
        std::stack<std::pair<btree_node* const*, std::size_t>> _path;
        std::size_t _index = 0;

        friend class B_tree;
        friend class btree_reverse_iterator;
        friend class btree_iterator;
        friend class btree_const_reverse_iterator;

        static bool same_path(
            std::stack<std::pair<btree_node* const*, std::size_t>> lhs,
            std::stack<std::pair<btree_node* const*, std::size_t>> rhs)
        {
            if (lhs.size() != rhs.size())
                return false;
            while (!lhs.empty())
            {
                if (lhs.top() != rhs.top())
                    return false;
                lhs.pop();
                rhs.pop();
            }
            return true;
        }

    public:
        using value_type = tree_data_type_const;
        using reference = const value_type&;
        using pointer = const value_type*;
        using iterator_category = std::bidirectional_iterator_tag;
        using difference_type = std::ptrdiff_t;
        using self = btree_const_iterator;

        btree_const_iterator(const btree_iterator& it) noexcept: _index(it._index)
        {
            std::vector<std::pair<btree_node* const*, std::size_t>> reversed;
            auto path = it._path;
            while (!path.empty())
            {
                reversed.push_back({path.top().first, path.top().second});
                path.pop();
            }
            for (auto iter = reversed.rbegin(); iter != reversed.rend(); ++iter)
                _path.push(*iter);
        }

        reference operator*() const noexcept
        {
            return reinterpret_cast<reference>((*_path.top().first)->_keys[_index]);
        }

        pointer operator->() const noexcept
        {
            return std::addressof(operator*());
        }

        self& operator++()
        {
            if (_path.empty())
                return *this;
            const auto original_path = _path;
            btree_node* node = *_path.top().first;
            if (node == nullptr)
                return *this;
            if (!node->_pointers.empty())
            {
                const std::size_t edge = _index + 1;

                if (edge < node->_pointers.size())
                {
                    auto link = &node->_pointers[edge];
                    _path.emplace(link, edge);
                    node = *link;

                    while (!node->_pointers.empty())
                    {
                        link = &node->_pointers[0];
                        node = *link;
                        _path.emplace(link, 0);
                    }

                    _index = 0;
                    return *this;
                }
            }

            if (_index + 1 < node->_keys.size())
            {
                ++_index;
                return *this;
            }
            const auto last_entry = _path.top();
            std::size_t edge = last_entry.second;
            _path.pop();
            while (!_path.empty())
            {
                btree_node* parent = *_path.top().first;
                if (edge < parent->_keys.size())
                {
                    _index = edge;
                    return *this;
                }
                edge = _path.top().second;
                _path.pop();
            }
            _path = original_path;
            _index = node->_keys.size();
            return *this;
        }

        self operator++(int)
        {
            self result(*this);
            ++(*this);
            return result;
        }

        self& operator--()
        {
            if (_path.empty())
                return *this;

            btree_node* node = *_path.top().first;
            if (node == nullptr)
                return *this;

            if (_index == node->_keys.size())
            {
                if (!node->_keys.empty())
                    --_index;
                return *this;
            }

            if (_index > 0)
            {
                --_index;
                return *this;
            }

            if (!node->_pointers.empty() && _index < node->_pointers.size())
            {
                const std::size_t edge = _index;
                btree_node* const* link = &node->_pointers[edge];
                _path.emplace(link, edge);
                node = *link;
                while (!node->_pointers.empty())
                {
                    const std::size_t right = node->_pointers.size() - 1;
                    link = &node->_pointers[right];
                    node = *link;
                    _path.emplace(link, right);
                }
                _index = node->_keys.size() - 1;
                return *this;
            }

            std::size_t edge = _path.top().second;
            _path.pop();
            while (!_path.empty())
            {
                if (edge > 0)
                {
                    _index = edge - 1;
                    return *this;
                }
                edge = _path.top().second;
                _path.pop();
            }
            _index = 0;
            return *this;
        }

        self operator--(int)
        {
            self result(*this);
            --(*this);
            return result;
        }

        bool operator==(const self& other) const noexcept
        {
            return _index == other._index && same_path(_path, other._path);
        }

        bool operator!=(const self& other) const noexcept
        {
            return !(*this == other);
        }

        std::size_t depth() const noexcept
        {
            return _path.empty() ? 0 : _path.size() - 1;
        }

        std::size_t current_node_keys_count() const noexcept
        {
            return _path.empty() ? 0 : (*_path.top().first)->_keys.size();
        }

        bool is_terminate_node() const noexcept
        {
            return !_path.empty() && (*_path.top().first)->_pointers.empty();
        }

        std::size_t index() const noexcept
        {
            return _index;
        }

        explicit btree_const_iterator(
            const std::stack<std::pair<btree_node* const*, std::size_t>>& path = {},
            std::size_t index = 0)
            : _path(path), _index(index)
        {
        }
    };

    class btree_reverse_iterator final
    {
        std::stack<std::pair<btree_node**, std::size_t>> _path;
        std::size_t _index = 0;

        friend class B_tree;
        friend class btree_iterator;
        friend class btree_const_iterator;
        friend class btree_const_reverse_iterator;

    public:
        using value_type = tree_data_type_const;
        using reference = value_type&;
        using pointer = value_type*;
        using iterator_category = std::bidirectional_iterator_tag;
        using difference_type = std::ptrdiff_t;
        using self = btree_reverse_iterator;

        btree_reverse_iterator(const btree_iterator& it) noexcept
            : _path(it._path), _index(it._index)
        {
        }

        operator btree_iterator() const noexcept
        {
            return btree_iterator(_path, _index);
        }

        reference operator*() const noexcept
        {
            return reinterpret_cast<reference>((*_path.top().first)->_keys[_index]);
        }

        pointer operator->() const noexcept
        {
            return std::addressof(operator*());
        }

        self& operator++()
        {
            btree_iterator forward(_path, _index);
            --forward;
            _path = forward._path;
            _index = forward._index;
            return *this;
        }

        self operator++(int)
        {
            self result(*this);
            ++(*this);
            return result;
        }

        self& operator--()
        {
            btree_iterator forward(_path, _index);
            ++forward;
            _path = forward._path;
            _index = forward._index;
            return *this;
        }

        self operator--(int)
        {
            self result(*this);
            --(*this);
            return result;
        }

        bool operator==(const self& other) const noexcept
        {
            return btree_iterator(_path, _index) == btree_iterator(other._path, other._index);
        }

        bool operator!=(const self& other) const noexcept
        {
            return !(*this == other);
        }

        std::size_t depth() const noexcept
        {
            return _path.empty() ? 0 : _path.size() - 1;
        }

        std::size_t current_node_keys_count() const noexcept
        {
            return _path.empty() ? 0 : (*_path.top().first)->_keys.size();
        }

        bool is_terminate_node() const noexcept
        {
            return !_path.empty() && (*_path.top().first)->_pointers.empty();
        }

        std::size_t index() const noexcept
        {
            return _index;
        }

        explicit btree_reverse_iterator(const std::stack<std::pair<btree_node**, std::size_t>>& path = {}, std::size_t index = 0)
            : _path(path), _index(index)
        {
        }
    };

    class btree_const_reverse_iterator final
    {
        std::stack<std::pair<btree_node* const*, std::size_t>> _path;
        std::size_t _index = 0;

        friend class B_tree;
        friend class btree_reverse_iterator;
        friend class btree_const_iterator;
        friend class btree_iterator;

    public:
        using value_type = tree_data_type_const;
        using reference = const value_type&;
        using pointer = const value_type*;
        using iterator_category = std::bidirectional_iterator_tag;
        using difference_type = std::ptrdiff_t;
        using self = btree_const_reverse_iterator;

        btree_const_reverse_iterator(const btree_reverse_iterator& it) noexcept
            : _index(it._index)
        {
            auto path = it._path;
            std::vector<std::pair<btree_node* const*, std::size_t>> reversed;
            while (!path.empty())
            {
                reversed.push_back({path.top().first, path.top().second});
                path.pop();
            }
            for (auto iter = reversed.rbegin(); iter != reversed.rend(); ++iter)
                _path.push(*iter);
        }

        operator btree_const_iterator() const noexcept
        {
            return btree_const_iterator(_path, _index);
        }

        reference operator*() const noexcept
        {
            return reinterpret_cast<reference>((*_path.top().first)->_keys[_index]);
        }

        pointer operator->() const noexcept
        {
            return std::addressof(operator*());
        }

        self& operator++()
        {
            btree_const_iterator forward(_path, _index);
            --forward;
            _path = forward._path;
            _index = forward._index;
            return *this;
        }

        self operator++(int)
        {
            self result(*this);
            ++(*this);
            return result;
        }

        self& operator--()
        {
            btree_const_iterator forward(_path, _index);
            ++forward;
            _path = forward._path;
            _index = forward._index;
            return *this;
        }

        self operator--(int)
        {
            self result(*this);
            --(*this);
            return result;
        }

        bool operator==(const self& other) const noexcept
        {
            return btree_const_iterator(_path, _index) == btree_const_iterator(other._path, other._index);
        }

        bool operator!=(const self& other) const noexcept
        {
            return !(*this == other);
        }

        std::size_t depth() const noexcept
        {
            return _path.empty() ? 0 : _path.size() - 1;
        }

        std::size_t current_node_keys_count() const noexcept
        {
            return _path.empty() ? 0 : (*_path.top().first)->_keys.size();
        }

        bool is_terminate_node() const noexcept
        {
            return !_path.empty() && (*_path.top().first)->_pointers.empty();
        }

        std::size_t index() const noexcept
        {
            return _index;
        }

        explicit btree_const_reverse_iterator(
            const std::stack<std::pair<btree_node* const*, std::size_t>>& path = {},
            std::size_t index = 0)
            : _path(path), _index(index)
        {
        }
    };

    friend class btree_iterator;
    friend class btree_const_iterator;
    friend class btree_reverse_iterator;
    friend class btree_const_reverse_iterator;

    tvalue& at(const tkey& key)
    {
        btree_iterator it = find(key);
        if (it == end())
            throw key_not_found("B_tree::at: key not found");
        return it->second;
    }

    const tvalue& at(const tkey& key) const
    {
        btree_const_iterator it = find(key);
        if (it == cend())
            throw key_not_found("B_tree::at: key not found");
        return it->second;
    }

    tvalue& operator[](const tkey& key)
    {
        auto result = emplace(key, tvalue{});
        return result.first->second;
    }

    tvalue& operator[](tkey&& key)
    {
        auto result = emplace(std::move(key), tvalue{});
        return result.first->second;
    }

    btree_iterator begin()
    {
        if (_root == nullptr)
            return end();
        std::stack<std::pair<btree_node**, std::size_t>> path;
        btree_node** link = &_root;
        btree_node* node = _root;
        path.emplace(link, 0);
        while (!node->_pointers.empty())
        {
            link = &node->_pointers[0];
            node = *link;
            path.emplace(link, 0);
        }
        return btree_iterator(path, 0);
    }

    btree_iterator end()
    {
        if (_root == nullptr)
            return btree_iterator();
        std::stack<std::pair<btree_node**, std::size_t>> path;
        btree_node** link = &_root;
        btree_node* node = _root;
        path.emplace(link, 0);
        while (!node->_pointers.empty())
        {
            const std::size_t edge = node->_pointers.size() - 1;
            link = &node->_pointers[edge];
            node = *link;
            path.emplace(link, edge);
        }
        return btree_iterator(path, node->_keys.size());
    }

    btree_const_iterator begin() const
    {
        return cbegin();
    }

    btree_const_iterator end() const
    {
        return cend();
    }

    btree_const_iterator cbegin() const
    {
        if (_root == nullptr)
            return cend();
        std::stack<std::pair<btree_node* const*, std::size_t>> path;
        btree_node* const* link = &_root;
        btree_node* node = *link;
        path.emplace(link, 0);
        while (!node->_pointers.empty())
        {
            link = &node->_pointers[0];
            node = *link;
            path.emplace(link, 0);
        }
        return btree_const_iterator(path, 0);
    }

    btree_const_iterator cend() const
    {
        if (_root == nullptr)
            return btree_const_iterator();
        std::stack<std::pair<btree_node* const*, std::size_t>> path;
        btree_node* const* link = &_root;
        btree_node* node = *link;
        path.emplace(link, 0);
        while (!node->_pointers.empty())
        {
            const std::size_t edge = node->_pointers.size() - 1;
            link = &node->_pointers[edge];
            node = *link;
            path.emplace(link, edge);
        }
        return btree_const_iterator(path, node->_keys.size());
    }

    btree_reverse_iterator rbegin()
    {
        if (_root == nullptr)
            return rend();
        std::stack<std::pair<btree_node**, std::size_t>> path;
        btree_node** link = &_root;
        btree_node* node = _root;
        path.emplace(link, 0);
        while (!node->_pointers.empty())
        {
            const std::size_t edge = node->_pointers.size() - 1;
            link = &node->_pointers[edge];
            node = *link;
            path.emplace(link, edge);
        }
        return btree_reverse_iterator(path, node->_keys.size() - 1);
    }

    btree_reverse_iterator rend()
    {
        if (_root == nullptr)
            return btree_reverse_iterator();
        std::stack<std::pair<btree_node**, std::size_t>> path;
        btree_node** link = &_root;
        btree_node* node = _root;
        path.emplace(link, 0);
        while (!node->_pointers.empty())
        {
            link = &node->_pointers[0];
            node = *link;
            path.emplace(link, 0);
        }
        return btree_reverse_iterator(path, std::numeric_limits<std::size_t>::max());
    }

    btree_const_reverse_iterator rbegin() const
    {
        return crbegin();
    }

    btree_const_reverse_iterator rend() const
    {
        return crend();
    }

    btree_const_reverse_iterator crbegin() const
    {
        if (_root == nullptr)
            return crend();
        std::stack<std::pair<btree_node* const*, std::size_t>> path;
        btree_node* const* link = &_root;
        btree_node* node = *link;
        path.emplace(link, 0);
        while (!node->_pointers.empty())
        {
            const std::size_t edge = node->_pointers.size() - 1;
            link = &node->_pointers[edge];
            node = *link;
            path.emplace(link, edge);
        }
        return btree_const_reverse_iterator(path, node->_keys.size() - 1);
    }

    btree_const_reverse_iterator crend() const
    {
        if (_root == nullptr)
            return btree_const_reverse_iterator();
        std::stack<std::pair<btree_node* const*, std::size_t>> path;
        btree_node* const* link = &_root;
        btree_node* node = *link;
        path.emplace(link, 0);
        while (!node->_pointers.empty())
        {
            link = &node->_pointers[0];
            node = *link;
            path.emplace(link, 0);
        }
        return btree_const_reverse_iterator(path, std::numeric_limits<std::size_t>::max());
    }

    std::size_t size() const noexcept
    {
        return _size;
    }

    bool empty() const noexcept
    {
        return _size == 0;
    }

    btree_iterator find(const tkey& key)
    {
        std::vector<std::pair<btree_node**, std::size_t>> path;
        if (!find_path(key, path))
            return end();
        const std::size_t index = lower_index(*path.back().first, key);
        std::stack<std::pair<btree_node**, std::size_t>> result;
        vector_to_stack(path, result);
        return btree_iterator(result, index);
    }

    btree_const_iterator find(const tkey& key) const
    {
        if (_root == nullptr)
            return cend();
        std::vector<std::pair<btree_node* const*, std::size_t>> path;
        btree_node* const* link = &_root;
        btree_node* node = *link;
        std::size_t edge = 0;
        while (node != nullptr)
        {
            const std::size_t index = lower_index(node, key);
            path.emplace_back(link, edge);
            if (index < node->_keys.size() && equivalent_keys(node->_keys[index].first, key))
            {
                std::stack<std::pair<btree_node* const*, std::size_t>> result;
                for (const auto& item : path)
                    result.push(item);
                return btree_const_iterator(result, index);
            }
            if (node->_pointers.empty())
                return cend();
            edge = index;
            link = &node->_pointers[index];
            node = *link;
        }
        return cend();
    }

    btree_iterator lower_bound(const tkey& key)
    {
        std::vector<std::pair<btree_node**, std::size_t>> path;
        std::size_t index = 0;
        if (!make_bound_path(key, false, path, index))
            return end();
        std::stack<std::pair<btree_node**, std::size_t>> result;
        vector_to_stack(path, result);
        return btree_iterator(result, index);
    }

    btree_const_iterator lower_bound(const tkey& key) const
    {
        if (_root == nullptr)
            return cend();
        std::vector<std::pair<btree_node* const*, std::size_t>> current;
        std::vector<std::pair<btree_node* const*, std::size_t>> candidate;
        std::size_t candidate_index = 0;
        btree_node* const* link = &_root;
        btree_node* node = *link;
        std::size_t edge = 0;
        while (node != nullptr)
        {
            current.emplace_back(link, edge);
            const std::size_t i = lower_index(node, key);
            if (i < node->_keys.size())
            {
                candidate = current;
                candidate_index = i;
            }
            if (node->_pointers.empty())
                break;
            edge = i;
            link = &node->_pointers[i];
            node = *link;
        }
        if (candidate.empty())
            return cend();
        std::stack<std::pair<btree_node* const*, std::size_t>> result;
        for (const auto& item : candidate)
            result.push(item);
        return btree_const_iterator(result, candidate_index);
    }

    btree_iterator upper_bound(const tkey& key)
    {
        std::vector<std::pair<btree_node**, std::size_t>> path;
        std::size_t index = 0;
        if (!make_bound_path(key, true, path, index))
            return end();
        std::stack<std::pair<btree_node**, std::size_t>> result;
        vector_to_stack(path, result);
        return btree_iterator(result, index);
    }

    btree_const_iterator upper_bound(const tkey& key) const
    {
        if (_root == nullptr)
            return cend();
        std::vector<std::pair<btree_node* const*, std::size_t>> current;
        std::vector<std::pair<btree_node* const*, std::size_t>> candidate;
        std::size_t candidate_index = 0;
        btree_node* const* link = &_root;
        btree_node* node = *link;
        std::size_t edge = 0;
        while (node != nullptr)
        {
            current.emplace_back(link, edge);
            const std::size_t i = upper_index(node, key);
            if (i < node->_keys.size())
            {
                candidate = current;
                candidate_index = i;
            }
            if (node->_pointers.empty())
                break;
            edge = i;
            link = &node->_pointers[i];
            node = *link;
        }
        if (candidate.empty())
            return cend();
        std::stack<std::pair<btree_node* const*, std::size_t>> result;
        for (const auto& item : candidate)
            result.push(item);
        return btree_const_iterator(result, candidate_index);
    }

    bool contains(const tkey& key) const
    {
        return find(key) != cend();
    }

    void clear() noexcept
    {
        destroy_subtree(_root);
        _root = nullptr;
        _size = 0;
    }

    std::pair<btree_iterator, bool> insert(const tree_data_type& data)
    {
        btree_iterator existing = find(data.first);
        if (existing != end())
            return {existing, false};
        insert_new(data);
        return {find(data.first), true};
    }

    std::pair<btree_iterator, bool> insert(tree_data_type&& data)
    {
        const tkey key = data.first;
        btree_iterator existing = find(key);
        if (existing != end())
            return {existing, false};
        insert_new(std::move(data));
        return {find(key), true};
    }

    template <typename... Args>
    std::pair<btree_iterator, bool> emplace(Args&&... args)
    {
        tree_data_type data(std::forward<Args>(args)...);
        return insert(std::move(data));
    }

    btree_iterator insert_or_assign(const tree_data_type& data)
    {
        btree_iterator it = find(data.first);
        if (it != end())
        {
            it->second = data.second;
            return it;
        }
        return insert(data).first;
    }

    btree_iterator insert_or_assign(tree_data_type&& data)
    {
        btree_iterator it = find(data.first);
        if (it != end())
        {
            it->second = std::move(data.second);
            return it;
        }
        return insert(std::move(data)).first;
    }
    template <typename... Args>
    btree_iterator emplace_or_assign(Args&&... args)
    {
        tree_data_type data(std::forward<Args>(args)...);
        return insert_or_assign(std::move(data));
    }
    btree_iterator erase(btree_iterator pos)
    {
        if (pos == end())
            return end();
        const tkey key = pos->first;
        btree_iterator next = pos;
        ++next;
        bool has_next = next != end();
        tkey next_key{};
        if (has_next)
            next_key = next->first;
        erase_existing(key);
        return has_next ? lower_bound(next_key) : end();
    }

    btree_iterator erase(btree_const_iterator pos)
    {
        if (pos == cend())
            return end();
        const tkey key = pos->first;
        btree_const_iterator next = pos;
        next++;
        bool has_next = next != cend();
        tkey next_key{};
        if (has_next)
            next_key = next->first;
        erase_existing(key);
        return has_next ? lower_bound(next_key) : end();
    }

    btree_iterator erase(btree_iterator beg, btree_iterator en)
    {
        if (beg == en)
            return end();
        std::vector<tkey> keys;
        for (auto it = beg; it != en; ++it)
            keys.push_back(it->first);
        bool has_after = en != end();
        tkey after_key{};
        if (has_after)
            after_key = en->first;
        for (const tkey& key : keys)
            erase_existing(key);
        return has_after ? lower_bound(after_key) : end();
    }

    btree_iterator erase(btree_const_iterator beg, btree_const_iterator en)
    {
        if (beg == en)
            return end();
        std::vector<tkey> keys;
        for (auto it = beg; it != en; ++it)
            keys.push_back(it->first);
        bool has_after = en != cend();
        tkey after_key{};
        if (has_after)
            after_key = en->first;
        for (const tkey& key : keys)
            erase_existing(key);
        return has_after ? lower_bound(after_key) : end();
    }

    btree_iterator erase(const tkey& key)
    {
        btree_iterator it = find(key);
        if (it == end())
            return end();
        return erase(it);
    }
};

template<
    std::input_iterator iterator,
    comparator<typename std::iterator_traits<iterator>::value_type::first_type> compare =
        std::less<typename std::iterator_traits<iterator>::value_type::first_type>,
    std::size_t t = 5,
    typename U>
B_tree(iterator, iterator, const compare& = compare(), pp_allocator<U> = pp_allocator<U>())
    -> B_tree<
        typename std::iterator_traits<iterator>::value_type::first_type,
        typename std::iterator_traits<iterator>::value_type::second_type,
        compare,
        t>;

template<
    typename tkey,
    typename tvalue,
    comparator<tkey> compare = std::less<tkey>,
    std::size_t t = 5,
    typename U>
B_tree(std::initializer_list<std::pair<tkey, tvalue>>, const compare& = compare(), pp_allocator<U> = pp_allocator<U>())
    -> B_tree<tkey, tvalue, compare, t>;

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t> bool compare_pairs(const std::pair<tkey, tvalue>& lhs, const std::pair<tkey, tvalue>& rhs)
{
    return std::less<tkey>{}(lhs.first, rhs.first);
}

template<typename tkey, typename tvalue, comparator<tkey> compare, std::size_t t>bool compare_keys(const tkey& lhs, const tkey& rhs)
{
    return std::less<tkey>{}(lhs, rhs);
}

#endif