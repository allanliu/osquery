/**
 *  Copyright (c) 2014-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under both the Apache 2.0 license (found in the
 *  LICENSE file in the root directory of this source tree) and the GPLv2 (found
 *  in the COPYING file in the root directory of this source tree).
 *  You may select, at your option, one of the above-listed licenses.
 */

#include <locale>

#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/trim.hpp>

#include <osquery/core.h>
#include <osquery/filesystem.h>
#include <osquery/logger.h>
#include <osquery/tables.h>

#include "osquery/events/linux/udev.h"
#include "osquery/tables/system/linux/pci_devices.h"

namespace osquery {
namespace tables {

const std::string kPCIKeySlot = "PCI_SLOT_NAME";
const std::string kPCIKeyClass = "ID_PCI_CLASS_FROM_DATABASE";
const std::string kPCIKeyVendor = "ID_VENDOR_FROM_DATABASE";
const std::string kPCIKeyModel = "ID_MODEL_FROM_DATABASE";
const std::string kPCIKeyID = "PCI_ID";
const std::string kPCIKeyDriver = "DRIVER";
const std::string kPCISubsysID = "PCI_SUBSYS_ID";

PciDB::PciDB(const std::string& path) {
  std::ifstream raw(path);
  if (raw.fail()) {
    LOG(ERROR) << "failed to read " << path;
    return;
  }

  std::string curVendor, curModel, line;
  while (std::getline(raw, line)) {
    if (line.size() < 7 || line.at(0) == '\n' || line.at(0) == '#') {
      continue;
    }

    switch (line.find_first_of("0123456789abcdef")) {
    case 0:
      // Vendor info.
      curVendor = line.substr(0, 4);

      // Once we get to illege vendor section we can stop since we're not
      // currently parsing device classes.
      if (curVendor == "ffff") {
        return;
      }

      db_[curVendor] = PciVendor{
          curVendor,
          // Bump 2 to account for whitespace separation..
          line.substr(6),
      };

      break;

    case 1:
      // Model info.
      if (db_.find(curVendor) != db_.end() && line.size() > 7) {
        curModel = line.substr(1, 4);

        db_[curVendor].models[curModel] = PciModel{
            curModel,
            // Bump 2 to account for whitespace separation.
            line.substr(7),
        };

        // TODO: remove else line.
      } else {
        VLOG(1) << "unexpected error while parsing pci.ids: current vendor ID "
                << curVendor << " does not exist in DB yet";
      }

      break;

    case 2:
      // Subsystem info;
      if (db_.find(curVendor) != db_.end() &&
          db_[curVendor].models.find(curModel) != db_[curVendor].models.end() &&
          line.size() > 11) {
        db_[curVendor].models[curModel].subsystemInfo[line.substr(2, 9)] =
            line.substr(12);

        // TODO: remove else line.
      } else {
        VLOG(1) << "unexpected error while parsing pci.ids: current vendor ID "
                << curVendor << " or model ID " << curModel
                << " does not exist in DB yet";
      }

      break;

    default:
      VLOG(1) << "unexpected pci.ids line format";
    }
  }
}

Status PciDB::getVendorName(const std::string& vendorID, std::string& vendor) {
  if (db_.find(vendorID) == db_.end()) {
    return Status(1, "Vendor ID does not exist");
  }

  vendor = db_[vendorID].name;
  return Status(0, "OK");
}

Status PciDB::getModel(const std::string& vendorID,
                       const std::string& modelID,
                       std::string& model,
                       const std::string& subsystemID) {
  if (db_.find(vendorID) == db_.end() ||
      db_[vendorID].models.find(modelID) == db_[vendorID].models.end()) {
    return Status(1, "Vendor ID or Model ID does not exist");
  }

  model = db_[vendorID].models[modelID].desc;

  if (subsystemID != "") {
    if (db_[vendorID].models[modelID].subsystemInfo.find(subsystemID) !=
        db_[vendorID].models[modelID].subsystemInfo.end()) {
      model.append("," +
                   db_[vendorID].models[modelID].subsystemInfo[subsystemID]);
    } else {
      VLOG(1) << "subsystem ID does not exist in system pci.ids: "
              << subsystemID;
    }
  }

  return Status(0, "OK");
}

QueryData genPCIDevices(QueryContext& context) {
  QueryData results;

  auto delUdev = [](udev* u) { udev_unref(u); };
  std::unique_ptr<udev, decltype(delUdev)> udev_handle(udev_new(), delUdev);
  if (udev_handle.get() == nullptr) {
    VLOG(1) << "Could not get udev handle";
    return results;
  }

  // Perform enumeration/search.
  auto delUdevEnum = [](udev_enumerate* e) { udev_enumerate_unref(e); };
  std::unique_ptr<udev_enumerate, decltype(delUdevEnum)> enumerate(
      udev_enumerate_new(udev_handle.get()), delUdevEnum);
  if (enumerate.get() == nullptr) {
    VLOG(1) << "Could not get udev_enumerate handle";
    return results;
  }

  PciDB pcidb;

  udev_enumerate_add_match_subsystem(enumerate.get(), "pci");
  udev_enumerate_scan_devices(enumerate.get());

  // Get list entries and iterate over entries.
  struct udev_list_entry *device_entries, *entry;
  device_entries = udev_enumerate_get_list_entry(enumerate.get());

  auto delUdevDevice = [](udev_device* d) { udev_device_unref(d); };
  udev_list_entry_foreach(entry, device_entries) {
    const char* path = udev_list_entry_get_name(entry);
    std::unique_ptr<udev_device, decltype(delUdevDevice)> device(
        udev_device_new_from_syspath(udev_handle.get(), path), delUdevDevice);
    if (device.get() == nullptr) {
      VLOG(1) << "Could not get device";
      return results;
    }

    Row r;
    r["pci_slot"] = UdevEventPublisher::getValue(device.get(), kPCIKeySlot);
    r["pci_class"] = UdevEventPublisher::getValue(device.get(), kPCIKeyClass);
    r["driver"] = UdevEventPublisher::getValue(device.get(), kPCIKeyDriver);
    r["vendor"] = UdevEventPublisher::getValue(device.get(), kPCIKeyVendor);
    r["model"] = UdevEventPublisher::getValue(device.get(), kPCIKeyModel);

    // VENDOR:MODEL ID is in the form of HHHH:HHHH.
    std::vector<std::string> ids;
    auto device_id = UdevEventPublisher::getValue(device.get(), kPCIKeyID);
    boost::split(ids, device_id, boost::is_any_of(":"));
    if (ids.size() == 2) {
      r["vendor_id"] = ids[0];
      r["model_id"] = ids[1];

      // Now that we know we have VENDOR and MODEL ID's, let's actually check on
      // the system PCI DB for descriptive information.
      // pci.ids hex IDs are all lower case, so we down case them.
      // TODO: should we just stoi instead? Which way is more performant.
      std::transform(
          ids[0].begin(), ids[0].end(), ids[0].begin(), [](unsigned char c) {
            return std::tolower(c);
          });
      std::transform(
          ids[1].begin(), ids[1].end(), ids[1].begin(), [](unsigned char c) {
            return std::tolower(c);
          });

      std::string vendor;
      if (pcidb.getVendorName(ids[0], vendor).ok()) {
        r["vendor"] = vendor;
      }

      // Try to enrich model with subsystem info.
      auto subsystemID =
          UdevEventPublisher::getValue(device.get(), kPCISubsysID);
      if (subsystemID.size() == 9) {
        subsystemID.at(4) = ' ';
      }
      std::transform(subsystemID.begin(),
                     subsystemID.end(),
                     subsystemID.begin(),
                     [](unsigned char c) { return std::tolower(c); });

      std::string model;
      if (pcidb.getModel(ids[0], ids[1], model, subsystemID).ok()) {
        r["model"] = model;
      }
    }

    // Set invalid vendor/model IDs to 0.
    if (r["vendor_id"].size() == 0) {
      r["vendor_id"] = "0";
    }

    if (r["model_id"].size() == 0) {
      r["model_id"] = "0";
    }

    results.push_back(r);
  }

  return results;
}
} // namespace tables
} // namespace osquery
