#include <cstring>
#include <typeinfo>

#include "BKE_node.h"
#include "SIM_node_tree.h"

#include "BLI_vector.h"
#include "BLI_string_ref.h"
#include "BLI_set.h"
#include "BLI_linear_allocator.h"
#include "BLI_color.h"
#include "BLI_string.h"
#include "BLI_array_cxx.h"

#include "PIL_time.h"

#include "BKE_context.h"

#include "DNA_space_types.h"

#include "UI_interface.h"

#include "../space_node/node_intern.h"

using BLI::Array;
using BLI::ArrayRef;
using BLI::LinearAllocator;
using BLI::rgba_f;
using BLI::Set;
using BLI::StringRef;
using BLI::StringRefNull;
using BLI::Vector;

class SocketDataType;
class BaseSocketDataType;
class ListSocketDataType;

enum class SocketTypeCategory {
  Base,
  List,
};

class SocketDataType {
 public:
  std::string m_ui_name;
  bNodeSocketType *m_socket_type;
  SocketTypeCategory m_category;

  SocketDataType(StringRef ui_name, bNodeSocketType *socket_type, SocketTypeCategory category)
      : m_ui_name(ui_name), m_socket_type(socket_type), m_category(category)
  {
  }

  bNodeSocket *build(bNodeTree &ntree,
                     bNode &node,
                     eNodeSocketInOut in_out,
                     StringRef identifier,
                     StringRef ui_name) const
  {
    return nodeAddSocket(
        &ntree, &node, in_out, m_socket_type->idname, identifier.data(), ui_name.data());
  }
};

class BaseSocketDataType : public SocketDataType {
 public:
  ListSocketDataType *m_list_type;

  BaseSocketDataType(StringRef ui_name, bNodeSocketType *socket_type)
      : SocketDataType(ui_name, socket_type, SocketTypeCategory::Base)
  {
  }
};

class ListSocketDataType : public SocketDataType {
 public:
  BaseSocketDataType *m_base_type;

  ListSocketDataType(StringRef ui_name, bNodeSocketType *socket_type)
      : SocketDataType(ui_name, socket_type, SocketTypeCategory::List)
  {
  }
};

class DataTypesInfo {
 private:
  Set<SocketDataType *> m_data_types;

 public:
  void add_data_type(SocketDataType *data_type)
  {
    m_data_types.add_new(data_type);
  }
};

static DataTypesInfo *socket_data_types;

static BaseSocketDataType *data_socket_float;
static BaseSocketDataType *data_socket_int;
static ListSocketDataType *data_socket_float_list;
static ListSocketDataType *data_socket_int_list;

class SocketDecl {
 protected:
  bNodeTree &m_ntree;
  bNode &m_node;
  uint m_amount;

 public:
  SocketDecl(bNodeTree &ntree, bNode &node, uint amount)
      : m_ntree(ntree), m_node(node), m_amount(amount)
  {
  }

  virtual ~SocketDecl();

  uint amount() const
  {
    return m_amount;
  }

  virtual bool sockets_are_correct(ArrayRef<bNodeSocket *> sockets) const = 0;

  virtual void build() const = 0;
};

SocketDecl::~SocketDecl()
{
}

class FixedTypeSocketDecl : public SocketDecl {
  eNodeSocketInOut m_in_out;
  SocketDataType &m_type;
  StringRefNull m_ui_name;
  StringRefNull m_identifier;

 public:
  FixedTypeSocketDecl(bNodeTree &ntree,
                      bNode &node,
                      eNodeSocketInOut in_out,
                      SocketDataType &type,
                      StringRefNull ui_name,
                      StringRefNull identifier)
      : SocketDecl(ntree, node, 1),
        m_in_out(in_out),
        m_type(type),
        m_ui_name(ui_name),
        m_identifier(identifier)
  {
  }

  bool sockets_are_correct(ArrayRef<bNodeSocket *> sockets) const override
  {
    if (sockets.size() != 1) {
      return false;
    }

    bNodeSocket *socket = sockets[0];
    if (socket->typeinfo != m_type.m_socket_type) {
      return false;
    }
    if (socket->name != m_ui_name) {
      return false;
    }
    if (socket->identifier != m_identifier) {
      return false;
    }
    return true;
  }

  void build() const override
  {
    m_type.build(m_ntree, m_node, m_in_out, m_identifier, m_ui_name);
  }
};

class NodeDecl {
 public:
  bNodeTree &m_ntree;
  bNode &m_node;
  Vector<SocketDecl *> m_inputs;
  Vector<SocketDecl *> m_outputs;

  NodeDecl(bNodeTree &ntree, bNode &node) : m_ntree(ntree), m_node(node)
  {
  }

  void build() const
  {
    for (SocketDecl *decl : m_inputs) {
      decl->build();
    }
    for (SocketDecl *decl : m_outputs) {
      decl->build();
    }
  }

  bool sockets_are_correct() const
  {
    if (!this->sockets_are_correct(m_node.inputs, m_inputs)) {
      return false;
    }
    if (!this->sockets_are_correct(m_node.outputs, m_outputs)) {
      return false;
    }
    return true;
  }

 private:
  bool sockets_are_correct(ListBase &sockets_list, ArrayRef<SocketDecl *> decls) const
  {
    Vector<bNodeSocket *, 10> sockets;
    LISTBASE_FOREACH (bNodeSocket *, socket, &sockets_list) {
      sockets.append(socket);
    }

    uint offset = 0;
    for (SocketDecl *decl : decls) {
      uint amount = decl->amount();
      if (offset + amount > sockets.size()) {
        return false;
      }
      ArrayRef<bNodeSocket *> sockets_for_decl = sockets.as_ref().slice(offset, amount);
      if (!decl->sockets_are_correct(sockets_for_decl)) {
        return false;
      }
      offset += amount;
    }
    if (offset != sockets.size()) {
      return false;
    }
    return true;
  }
};

template<typename T> static T *get_node_storage(bNode *node);
template<typename T> static const T *get_node_storage(const bNode *node);
template<typename T> static T *get_socket_storage(bNodeSocket *socket);

class NodeBuilder {
 private:
  LinearAllocator<> &m_allocator;
  NodeDecl &m_node_decl;

 public:
  NodeBuilder(LinearAllocator<> &allocator, NodeDecl &node_decl)
      : m_allocator(allocator), m_node_decl(node_decl)
  {
  }

  template<typename T> T *node_storage()
  {
    return get_node_storage<T>(&m_node_decl.m_node);
  }

  void fixed_input(StringRef identifier, StringRef ui_name, SocketDataType &type)
  {
    FixedTypeSocketDecl *decl = m_allocator.construct<FixedTypeSocketDecl>(
        m_node_decl.m_ntree,
        m_node_decl.m_node,
        SOCK_IN,
        type,
        m_allocator.copy_string(ui_name),
        m_allocator.copy_string(identifier));
    m_node_decl.m_inputs.append(decl);
  }

  void fixed_output(StringRef identifier, StringRef ui_name, SocketDataType &type)
  {
    FixedTypeSocketDecl *decl = m_allocator.construct<FixedTypeSocketDecl>(
        m_node_decl.m_ntree,
        m_node_decl.m_node,
        SOCK_OUT,
        type,
        m_allocator.copy_string(ui_name),
        m_allocator.copy_string(identifier));
    m_node_decl.m_outputs.append(decl);
  }

  void float_input(StringRef identifier, StringRef ui_name)
  {
    this->fixed_input(identifier, ui_name, *data_socket_float);
  }

  void int_input(StringRef identifier, StringRef ui_name)
  {
    this->fixed_input(identifier, ui_name, *data_socket_int);
  }

  void float_output(StringRef identifier, StringRef ui_name)
  {
    this->fixed_output(identifier, ui_name, *data_socket_float);
  }

  void int_output(StringRef identifier, StringRef ui_name)
  {
    this->fixed_output(identifier, ui_name, *data_socket_int);
  }
};

static void declare_test_node(NodeBuilder &builder)
{
  MyTestNodeStorage *storage = builder.node_storage<MyTestNodeStorage>();

  builder.float_input("id1", "ID 1");
  builder.int_input("id2", "ID 2");
  builder.int_input("id4", "ID 4");
  builder.float_output("id3", "ID 3");

  for (int i = 0; i < storage->x; i++) {
    builder.fixed_input(
        "id" + std::to_string(i), "Hello " + std::to_string(i), *data_socket_float_list);
  }
}

class SocketTypeDefinition {
 public:
  using DrawInNodeFn = std::function<void(struct bContext *C,
                                          struct uiLayout *layout,
                                          struct PointerRNA *ptr,
                                          struct PointerRNA *node_ptr,
                                          const char *text)>;
  using InitStorageFn = std::function<void *()>;
  using CopyStorageFn = std::function<void *(const void *)>;
  using FreeStorageFn = std::function<void(void *)>;
  template<typename T> using TypedInitStorageFn = std::function<void(T *)>;

 private:
  bNodeSocketType m_stype;
  DrawInNodeFn m_draw_in_node_fn;
  rgba_f m_color = {0.0f, 0.0f, 0.0f, 1.0f};
  std::string m_storage_struct_name;
  InitStorageFn m_init_storage_fn;
  CopyStorageFn m_copy_storage_fn;
  FreeStorageFn m_free_storage_fn;

 public:
  SocketTypeDefinition(StringRef idname)
  {
    memset(&m_stype, 0, sizeof(bNodeSocketType));
    idname.copy(m_stype.idname);
    m_stype.type = SOCK_CUSTOM;
    m_stype.draw = SocketTypeDefinition::draw_in_node;
    m_draw_in_node_fn = [](struct bContext *UNUSED(C),
                           struct uiLayout *layout,
                           struct PointerRNA *UNUSED(ptr),
                           struct PointerRNA *UNUSED(node_ptr),
                           const char *text) { uiItemL(layout, text, 0); };

    m_stype.draw_color = SocketTypeDefinition::get_draw_color;
    m_stype.free_self = [](bNodeSocketType *UNUSED(stype)) {};

    m_init_storage_fn = []() { return nullptr; };
    m_copy_storage_fn = [](const void *storage) {
      BLI_assert(storage == nullptr);
      UNUSED_VARS_NDEBUG(storage);
      return nullptr;
    };
    m_free_storage_fn = [](void *storage) {
      BLI_assert(storage == nullptr);
      UNUSED_VARS_NDEBUG(storage);
    };

    m_stype.init_fn = SocketTypeDefinition::init_socket;
    m_stype.copy_fn = SocketTypeDefinition::copy_socket;
    m_stype.free_fn = SocketTypeDefinition::free_socket;

    m_stype.userdata = (void *)this;
  }

  void set_color(rgba_f color)
  {
    m_color = color;
  }

  void add_dna_storage(StringRef struct_name,
                       InitStorageFn init_storage_fn,
                       CopyStorageFn copy_storage_fn,
                       FreeStorageFn free_storage_fn)
  {
    m_storage_struct_name = struct_name;
    m_init_storage_fn = init_storage_fn;
    m_copy_storage_fn = copy_storage_fn;
    m_free_storage_fn = free_storage_fn;
  }

  template<typename T>
  void add_dna_storage(StringRef struct_name, TypedInitStorageFn<T> init_storage_fn)
  {
    this->add_dna_storage(
        struct_name,
        [init_storage_fn]() {
          void *buffer = MEM_callocN(sizeof(T), __func__);
          init_storage_fn((T *)buffer);
          return buffer;
        },
        [](const void *buffer) {
          void *new_buffer = MEM_callocN(sizeof(T), __func__);
          memcpy(new_buffer, buffer, sizeof(T));
          return new_buffer;
        },
        [](void *buffer) { MEM_freeN(buffer); });
  }

  void add_draw_fn(DrawInNodeFn draw_in_node_fn)
  {
    m_draw_in_node_fn = draw_in_node_fn;
  }

  StringRefNull storage_struct_name() const
  {
    return m_storage_struct_name;
  }

  void register_type()
  {
    nodeRegisterSocketType(&m_stype);
  }

  static SocketTypeDefinition *get_from_socket(bNodeSocket *socket)
  {
    return (SocketTypeDefinition *)socket->typeinfo->userdata;
  }

 private:
  static void init_socket(bNodeTree *UNUSED(ntree), bNode *UNUSED(node), bNodeSocket *socket)
  {
    SocketTypeDefinition *def = get_from_socket(socket);
    socket->default_value = def->m_init_storage_fn();
  }

  static void copy_socket(bNodeTree *UNUSED(dst_ntree),
                          bNode *UNUSED(dst_node),
                          bNodeSocket *dst_socket,
                          const bNodeSocket *src_socket)
  {
    SocketTypeDefinition *def = get_from_socket(dst_socket);
    dst_socket->default_value = def->m_copy_storage_fn(src_socket->default_value);
  }

  static void free_socket(bNodeTree *UNUSED(ntree), bNode *UNUSED(node), bNodeSocket *socket)
  {
    SocketTypeDefinition *def = get_from_socket(socket);
    def->m_free_storage_fn(socket->default_value);
    socket->default_value = nullptr;
  }

  static void draw_in_node(struct bContext *C,
                           struct uiLayout *layout,
                           struct PointerRNA *ptr,
                           struct PointerRNA *node_ptr,
                           const char *text)
  {
    bNodeSocket *socket = (bNodeSocket *)ptr->data;
    SocketTypeDefinition *def = get_from_socket(socket);
    def->m_draw_in_node_fn(C, layout, ptr, node_ptr, text);
  }

  static void get_draw_color(struct bContext *UNUSED(C),
                             struct PointerRNA *ptr,
                             struct PointerRNA *UNUSED(node_ptr),
                             float *r_color)
  {
    bNodeSocket *socket = (bNodeSocket *)ptr->data;
    SocketTypeDefinition *def = get_from_socket(socket);
    memcpy(r_color, def->m_color, sizeof(rgba_f));
  }
};

class NodeTypeDefinition {
 public:
  using DeclareNodeFn = std::function<void(NodeBuilder &node_builder)>;
  using InitStorageFn = std::function<void *()>;
  using CopyStorageFn = std::function<void *(void *)>;
  using FreeStorageFn = std::function<void(void *)>;
  using DrawInNodeFn =
      std::function<void(struct uiLayout *layout, struct bContext *C, struct PointerRNA *ptr)>;
  template<typename T> using TypedInitStorageFn = std::function<void(T *)>;
  using CopyBehaviorFn = std::function<void(bNode *dst_node, const bNode *src_node)>;
  using LabelFn = std::function<void(bNodeTree *ntree, bNode *node, char *r_label, int maxlen)>;

 private:
  bNodeType m_ntype;
  DeclareNodeFn m_declare_node_fn;
  InitStorageFn m_init_storage_fn;
  CopyStorageFn m_copy_storage_fn;
  FreeStorageFn m_free_storage_fn;
  CopyBehaviorFn m_copy_node_fn;
  DrawInNodeFn m_draw_in_node_fn;
  LabelFn m_label_fn;

 public:
  NodeTypeDefinition(StringRef idname, StringRef ui_name, StringRef ui_description)
  {
    bNodeType *ntype = &m_ntype;

    memset(ntype, 0, sizeof(bNodeType));
    ntype->minwidth = 20;
    ntype->minheight = 20;
    ntype->maxwidth = 1000;
    ntype->maxheight = 1000;
    ntype->height = 100;
    ntype->width = 140;
    ntype->type = NODE_CUSTOM;

    idname.copy(ntype->idname);
    ui_name.copy(ntype->ui_name);
    ui_description.copy(ntype->ui_description);

    ntype->userdata = (void *)this;

    m_declare_node_fn = [](NodeBuilder &UNUSED(builder)) {};
    m_init_storage_fn = []() { return nullptr; };
    m_copy_storage_fn = [](void *storage) {
      BLI_assert(storage == nullptr);
      UNUSED_VARS_NDEBUG(storage);
      return nullptr;
    };
    m_free_storage_fn = [](void *storage) {
      BLI_assert(storage == nullptr);
      UNUSED_VARS_NDEBUG(storage);
    };
    m_draw_in_node_fn = [](struct uiLayout *UNUSED(layout),
                           struct bContext *UNUSED(C),
                           struct PointerRNA *UNUSED(ptr)) {};
    m_copy_node_fn = [](bNode *UNUSED(dst_node), const bNode *UNUSED(src_node)) {};

    ntype->poll = [](bNodeType *UNUSED(ntype), bNodeTree *UNUSED(ntree)) { return true; };
    ntype->initfunc = init_node;
    ntype->copyfunc = copy_node;
    ntype->freefunc = free_node;

    ntype->draw_buttons = [](struct uiLayout *layout, struct bContext *C, struct PointerRNA *ptr) {
      bNode *node = (bNode *)ptr->data;
      NodeTypeDefinition *def = type_from_node(node);
      def->m_draw_in_node_fn(layout, C, ptr);
    };

    ntype->draw_nodetype = node_draw_default;
    ntype->draw_nodetype_prepare = node_update_default;
    ntype->select_area_func = node_select_area_default;
    ntype->tweak_area_func = node_tweak_area_default;
    ntype->resize_area_func = node_resize_area_default;
    ntype->draw_buttons_ex = nullptr;
  }

  void add_declaration(DeclareNodeFn declare_fn)
  {
    m_declare_node_fn = declare_fn;
  }

  void add_dna_storage(StringRef struct_name,
                       InitStorageFn init_storage_fn,
                       CopyStorageFn copy_storage_fn,
                       FreeStorageFn free_storage_fn)
  {
    struct_name.copy(m_ntype.storagename);
    m_init_storage_fn = init_storage_fn;
    m_copy_storage_fn = copy_storage_fn;
    m_free_storage_fn = free_storage_fn;
  }

  template<typename T>
  void add_dna_storage(StringRef struct_name, TypedInitStorageFn<T> init_storage_fn)
  {
    this->add_dna_storage(
        struct_name,
        [init_storage_fn]() {
          void *buffer = MEM_callocN(sizeof(T), __func__);
          init_storage_fn((T *)buffer);
          return buffer;
        },
        [](void *buffer) {
          void *new_buffer = MEM_callocN(sizeof(T), __func__);
          memcpy(new_buffer, buffer, sizeof(T));
          return new_buffer;
        },
        [](void *buffer) { MEM_freeN(buffer); });
  }

  void add_copy_behavior(CopyBehaviorFn copy_fn)
  {
    m_copy_node_fn = copy_fn;
  }

  template<typename T>
  void add_copy_behavior(std::function<void(T *dst_storage, const T *src_storage)> copy_fn)
  {
    this->add_copy_behavior([copy_fn](bNode *dst_node, const bNode *src_node) {
      T *dst_storage = get_node_storage<T>(dst_node);
      const T *src_storage = get_node_storage<T>(src_node);
      copy_fn(dst_storage, src_storage);
    });
  }

  void add_draw_fn(DrawInNodeFn draw_fn)
  {
    m_draw_in_node_fn = draw_fn;
  }

  void add_label_fn(LabelFn label_fn)
  {
    m_ntype.labelfunc = node_label;
    m_label_fn = label_fn;
  }

  void register_type()
  {
    nodeRegisterType(&m_ntype);
  }

  static void declare_node(bNode *node, NodeBuilder &builder)
  {
    NodeTypeDefinition *def = type_from_node(node);
    def->m_declare_node_fn(builder);
  }

 private:
  static NodeTypeDefinition *type_from_node(bNode *node)
  {
    return (NodeTypeDefinition *)node->typeinfo->userdata;
  }

  static void init_node(bNodeTree *ntree, bNode *node)
  {
    NodeTypeDefinition *def = type_from_node(node);

    LinearAllocator<> allocator;
    NodeDecl node_decl{*ntree, *node};
    NodeBuilder node_builder{allocator, node_decl};
    node->storage = def->m_init_storage_fn();
    def->m_declare_node_fn(node_builder);
    node_decl.build();
  }

  static void copy_node(bNodeTree *UNUSED(dst_ntree), bNode *dst_node, const bNode *src_node)
  {
    BLI_assert(dst_node->typeinfo == src_node->typeinfo);
    NodeTypeDefinition *def = type_from_node(dst_node);

    dst_node->storage = def->m_copy_storage_fn(src_node->storage);
    def->m_copy_node_fn(dst_node, src_node);
  }

  static void free_node(bNode *node)
  {
    NodeTypeDefinition *def = type_from_node(node);
    def->m_free_storage_fn(node->storage);
    node->storage = nullptr;
  }

  static void node_label(bNodeTree *ntree, bNode *node, char *r_label, int maxlen)
  {
    NodeTypeDefinition *def = type_from_node(node);
    def->m_label_fn(ntree, node, r_label, maxlen);
  }
};

template<typename T> static T *get_node_storage(bNode *node)
{
#ifdef DEBUG
  const char *type_name = typeid(T).name();
  const char *expected_name = node->typeinfo->storagename;
  BLI_assert(strstr(type_name, expected_name));
#endif
  return (T *)node->storage;
}

template<typename T> static const T *get_node_storage(const bNode *node)
{
#ifdef DEBUG
  const char *type_name = typeid(T).name();
  const char *expected_name = node->typeinfo->storagename;
  BLI_assert(strstr(type_name, expected_name));
#endif
  return (const T *)node->storage;
}

template<typename T> static T *get_socket_storage(bNodeSocket *socket)
{
#ifdef DEBUG
  const char *type_name = typeid(T).name();
  const char *expected_name =
      SocketTypeDefinition::get_from_socket(socket)->storage_struct_name().data();
  BLI_assert(strstr(type_name, expected_name));
#endif
  return (T *)socket->default_value;
}

void register_node_type_my_test_node()
{
  {
    static NodeTypeDefinition ntype("MyTestNode", "My Test Node", "My Description");
    ntype.add_declaration(declare_test_node);
    ntype.add_dna_storage<MyTestNodeStorage>("MyTestNodeStorage",
                                             [](MyTestNodeStorage *storage) { storage->x = 3; });
    ntype.add_copy_behavior<MyTestNodeStorage>(
        [](MyTestNodeStorage *dst_storage, const MyTestNodeStorage *UNUSED(src_storage)) {
          dst_storage->x += 1;
        });
    ntype.add_draw_fn([](uiLayout *layout, struct bContext *UNUSED(C), struct PointerRNA *ptr) {
      bNode *node = (bNode *)ptr->data;
      MyTestNodeStorage *storage = (MyTestNodeStorage *)node->storage;
      uiBut *but = uiDefButI(uiLayoutGetBlock(layout),
                             UI_BTYPE_NUM,
                             0,
                             "X value",
                             0,
                             0,
                             50,
                             50,
                             &storage->x,
                             -1000,
                             1000,
                             3,
                             20,
                             "my x value");
      uiItemL(layout, "Hello World", 0);
      UI_but_func_set(
          but,
          [](bContext *C, void *UNUSED(arg1), void *UNUSED(arg2)) {
            bNodeTree *ntree = CTX_wm_space_node(C)->edittree;
            ntree->update = NTREE_UPDATE;
            ntreeUpdateTree(CTX_data_main(C), ntree);
          },
          nullptr,
          nullptr);
    });

    ntype.register_type();
  }
  {
    static NodeTypeDefinition ntype("MyTestNode2", "Node 2", "Description");
    ntype.add_declaration([](NodeBuilder &node_builder) {
      node_builder.float_input("a", "A");
      node_builder.float_input("b", "B");
      node_builder.float_output("result", "Result");
    });
    ntype.add_label_fn([](bNodeTree *UNUSED(ntree), bNode *node, char *r_label, int maxlen) {
      if (node->flag & NODE_HIDDEN) {
        BLI_strncpy(r_label, "Custom Label", maxlen);
      }
    });
    ntype.register_type();
  }
}

void init_socket_data_types()
{
  {
    static SocketTypeDefinition stype("NodeSocketFloatList");
    stype.set_color({0.63, 0.63, 0.63, 0.5});
    stype.register_type();
  }
  {
    static SocketTypeDefinition stype("NodeSocketIntList");
    stype.set_color({0.06, 0.52, 0.15, 0.5});
    stype.register_type();
  }
  {
    static SocketTypeDefinition stype("MyFloatSocket");
    stype.set_color({1, 1, 1, 1});
    stype.add_dna_storage<bNodeSocketValueFloat>(
        "bNodeSocketValueFloat", [](bNodeSocketValueFloat *storage) { storage->value = 11.5f; });
    stype.add_draw_fn([](bContext *UNUSED(C),
                         uiLayout *layout,
                         PointerRNA *ptr,
                         PointerRNA *UNUSED(node_ptr),
                         const char *UNUSED(text)) {
      bNodeSocket *socket = (bNodeSocket *)ptr->data;
      bNodeSocketValueFloat *storage = get_socket_storage<bNodeSocketValueFloat>(socket);
      uiDefButF(uiLayoutGetBlock(layout),
                UI_BTYPE_NUM,
                0,
                "My Value",
                0,
                0,
                150,
                30,
                &storage->value,
                -1000,
                1000,
                3,
                20,
                "my x value");
    });
    stype.register_type();
  }

  data_socket_float = new BaseSocketDataType("Float", nodeSocketTypeFind("MyFloatSocket"));
  data_socket_int = new BaseSocketDataType("Integer", nodeSocketTypeFind("NodeSocketInt"));
  data_socket_float_list = new ListSocketDataType("Float List",
                                                  nodeSocketTypeFind("NodeSocketFloatList"));
  data_socket_int_list = new ListSocketDataType("Integer List",
                                                nodeSocketTypeFind("NodeSocketIntList"));

  data_socket_float->m_list_type = data_socket_float_list;
  data_socket_float_list->m_base_type = data_socket_float;
  data_socket_int->m_list_type = data_socket_int_list;
  data_socket_int_list->m_base_type = data_socket_int;

  socket_data_types = new DataTypesInfo();
  socket_data_types->add_data_type(data_socket_float);
  socket_data_types->add_data_type(data_socket_int);
  socket_data_types->add_data_type(data_socket_float_list);
  socket_data_types->add_data_type(data_socket_int_list);
}

void free_socket_data_types()
{
  delete socket_data_types;
  delete data_socket_float;
  delete data_socket_int;
  delete data_socket_float_list;
  delete data_socket_int_list;
}

void update_sim_node_tree(bNodeTree *ntree)
{
  Vector<bNode *> nodes;
  for (bNode *node : BLI::IntrusiveListBaseWrapper<bNode>(ntree->nodes)) {
    nodes.append(node);
  }

  LinearAllocator<> allocator;

  for (bNode *node : nodes) {
    NodeDecl node_decl{*ntree, *node};
    NodeBuilder builder{allocator, node_decl};
    NodeTypeDefinition::declare_node(node, builder);

    if (!node_decl.sockets_are_correct()) {
      std::cout << "Rebuild\n";
      nodeRemoveAllSockets(ntree, node);
      node_decl.build();
    }
    else {
      std::cout << "Don't rebuild\n";
    }
  }
}