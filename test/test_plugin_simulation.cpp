#include <gtest/gtest.h>
#include "irods/irods_resource_plugin.hpp"
#include "irods/irods_resource_constants.hpp"
#include "irods/irods_file_object.hpp"
#include "irods/irods_erasure_coding_error_codes.hpp"
#include <boost/smart_ptr/make_shared.hpp>
#include <vector>
#include <map>
#include <memory>

// Mock iRODS Resource for children
class MockLeafResource : public irods::resource {
public:
    MockLeafResource(const std::string& name) : irods::resource(name, "") {
        add_operation(irods::RESOURCE_OP_CREATE, std::function<irods::error(irods::plugin_context&)>(
            [this](irods::plugin_context& ctx) {
                auto fco = boost::dynamic_pointer_cast<irods::file_object>(ctx.fco());
                if (fco) created_paths.push_back(fco->physical_path());
                return SUCCESS();
            }));
        
        add_operation<void*, int>(irods::RESOURCE_OP_WRITE, std::function<irods::error(irods::plugin_context&, void*, int)>(
            [this](irods::plugin_context& ctx, void* buf, int len) {
                total_written += len;
                // Store fragments for read simulation
                auto fco = boost::dynamic_pointer_cast<irods::file_object>(ctx.fco());
                if (fco) {
                    auto& storage = fragment_storage[fco->physical_path()];
                    storage.insert(storage.end(), static_cast<std::byte*>(buf), static_cast<std::byte*>(buf) + len);
                }
                return SUCCESS();
            }));

        add_operation<void*, int>(irods::RESOURCE_OP_READ, std::function<irods::error(irods::plugin_context&, void*, int)>(
            [this](irods::plugin_context& ctx, void* buf, int len) {
                auto fco = boost::dynamic_pointer_cast<irods::file_object>(ctx.fco());
                if (fco && !fail_read) {
                    auto& storage = fragment_storage[fco->physical_path()];
                    if (current_read_offset + len <= storage.size()) {
                        std::memcpy(buf, storage.data() + current_read_offset, len);
                        current_read_offset += len;
                        return SUCCESS();
                    }
                }
                return ERROR(SYS_COPY_LEN_ERR, "Read failed on mock leaf");
            }));

        add_operation<off_t, int>(irods::RESOURCE_OP_LSEEK, std::function<irods::error(irods::plugin_context&, off_t ofs, int whence)>(
            [this](irods::plugin_context& ctx, off_t ofs, int whence) {
                if (whence == SEEK_SET) current_read_offset = ofs;
                return SUCCESS();
            }));

        add_operation(irods::RESOURCE_OP_CLOSE, std::function<irods::error(irods::plugin_context&)>(
            [this](irods::plugin_context& ctx) {
                current_read_offset = 0;
                return SUCCESS();
            }));
    }

    std::vector<std::string> created_paths;
    size_t total_written = 0;
    std::map<std::string, std::vector<std::byte>> fragment_storage;
    size_t current_read_offset = 0;
    bool fail_read = false;
};

// Access the internal plugin factory
extern "C" irods::resource* plugin_factory(const std::string&, const std::string&);

class ErasureCodingPluginTest : public ::testing::Test {
protected:
    void SetUp() override {
        ec_resc_ = plugin_factory("ec_resc", "");
        
        for (int i = 0; i < 6; ++i) {
            std::string name = "mock_leaf_" + std::to_string(i);
            auto leaf = boost::shared_ptr<MockLeafResource>(new MockLeafResource(name));
            leaves_.push_back(leaf);
            child_map_[name] = std::make_pair(std::string(""), boost::static_pointer_cast<irods::resource>(leaf));
        }

        ec_resc_->set_property<std::string>(irods::RESOURCE_NAME, "ec_resc");
        ec_resc_->set_property<irods::resource_child_map*>(irods::RESC_CHILD_MAP_PROP, &child_map_);
        ec_resc_->set_property<std::string>(irods::RESOURCE_CONTEXT, "k=4;m=2;backend=liberasurecode");

        irods::error ret = ec_resc_->start_operation();
        ASSERT_TRUE(ret.ok()) << ret.result();
    }

    irods::resource* ec_resc_;
    irods::resource_child_map child_map_;
    std::vector<boost::shared_ptr<MockLeafResource>> leaves_;
};

TEST_F(ErasureCodingPluginTest, FullWriteReadLifecycle) {
    auto fco = boost::make_shared<irods::file_object>(
        nullptr, "/tempZone/home/rods/test.dat", "/tmp/test.dat", "ec_resc", 0, 0644, 0);

    // 1. CREATE & WRITE
    auto ret = ec_resc_->call(nullptr, irods::RESOURCE_OP_CREATE, fco);
    ASSERT_TRUE(ret.ok());

    size_t chunk_size = 32 * 1024 * 1024;
    std::vector<std::byte> original_data(chunk_size);
    for (size_t i = 0; i < chunk_size; ++i) original_data[i] = static_cast<std::byte>(i % 256);
    
    ret = ec_resc_->call<void*, int>(nullptr, irods::RESOURCE_OP_WRITE, fco, original_data.data(), static_cast<int>(original_data.size()));
    ASSERT_TRUE(ret.ok());

    ret = ec_resc_->call(nullptr, irods::RESOURCE_OP_CLOSE, fco);
    ASSERT_TRUE(ret.ok());

    // 2. OPEN & READ
    ret = ec_resc_->call(nullptr, irods::RESOURCE_OP_OPEN, fco);
    ASSERT_TRUE(ret.ok());

    std::vector<std::byte> read_buffer(chunk_size);
    ret = ec_resc_->call<void*, int>(nullptr, irods::RESOURCE_OP_READ, fco, read_buffer.data(), static_cast<int>(read_buffer.size()));
    ASSERT_TRUE(ret.ok()) << ret.result();

    // 3. VERIFY
    printf("Original size: %zu, Read size: %zu\n", original_data.size(), read_buffer.size());
    size_t first_mismatch = 0;
    bool found_mismatch = false;
    for (size_t i = 0; i < original_data.size(); ++i) {
        if (original_data[i] != read_buffer[i]) {
            first_mismatch = i;
            found_mismatch = true;
            break;
        }
    }
    if (found_mismatch) {
        printf("First mismatch at index %zu: expected %02x, got %02x\n", 
               first_mismatch, (int)original_data[first_mismatch], (int)read_buffer[first_mismatch]);
    }
    ASSERT_TRUE(std::equal(original_data.begin(), original_data.end(), read_buffer.begin()));

    // 4. SIMULATE FAILURE & RECOVERY
    // Drop one data fragment (leaf 0)
    leaves_[0]->fail_read = true;
    
    // We need to re-open to reset offsets for the next read
    ret = ec_resc_->call(nullptr, irods::RESOURCE_OP_CLOSE, fco);
    ret = ec_resc_->call(nullptr, irods::RESOURCE_OP_OPEN, fco);
    
    std::vector<std::byte> recovery_buffer(chunk_size);
    ret = ec_resc_->call<void*, int>(nullptr, irods::RESOURCE_OP_READ, fco, recovery_buffer.data(), static_cast<int>(recovery_buffer.size()));
    ASSERT_TRUE(ret.ok()) << "Recovery read failed: " << ret.result();
    
    // Verify recovered data matches original
    ASSERT_TRUE(std::equal(original_data.begin(), original_data.end(), recovery_buffer.begin()));
}
