#include "database.hpp"
#include <boost/lexical_cast.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

namespace json
{
	database::database(std::string sSetPath)
	{
		mtx.lock();
		sPath = sSetPath;
		data.parseFile(sPath);
		if(!data.exists("config")){
			data["config"]["autoSave"] = true;
			data["config"]["pretty"] = false;
			data["config"]["maxRevisions"] = 0;

		}
		if(!data.exists("sequence")){
			data["sequence"] = 0;
		}
		mtx.unlock();
	}

	database::~database()
	{
		save();
	}
	document database::save()
	{
		mtx.lock();
		document ret;
		ret["success"] = data.writeFile(sPath, data["config"]["pretty"].boolean());
		mtx.unlock();
		return ret;
	}

	std::string database::generateUUID()
	{
		static boost::uuids::basic_random_generator<boost::mt19937> gen;
		boost::uuids::uuid u = gen();
//		boost::uuids::uuid u = boost::uuids::random_generator()(); // initialize uuid

		return to_string(u);
	}

	document database::getConfig()
	{
		mtx.lock();
		document ret = data["config"];
		mtx.unlock();
		return ret;
	}

	document database::setConfig(document jSetConfig)
	{
		mtx.lock();
		document ret;
		data["config"] = jSetConfig;
		if(data["config"]["autoSave"].boolean()){
			ret = save();
		}
		mtx.unlock();
		return ret;
	}

	document database::getConfigValue(std::string sKey)
	{
		mtx.lock();
		document ret = data["config"][sKey];
		mtx.unlock();
		return ret;
	}

	document database::setConfigValue(std::string sKey, atom aValue)
	{
		mtx.lock();
		document ret;
		data["config"][sKey] = aValue;
		if(data["config"]["autoSave"].boolean()){
			ret = save();
		}
		mtx.unlock();
		return ret;
	}

	document database::getDocument(std::string id, std::string rev)
	{
		mtx.lock();
		document ret;
		if(data["data"].exists(id)){
			if(!rev.empty()){
				ret = data["data"][id]["docs"][rev];
			} else {
				iterator it = data["data"][id]["revs"].begin();
				if(it != data["data"][id]["revs"].end()){
					ret = data["data"][id]["docs"][(*it).string()];
				}
			}
		}
		mtx.unlock();
		return ret;
	}

	document database::setDocument(document doc)
	{
		mtx.lock();
		document ret;
		bool bOk = true;;
		if(doc.exists("_id")){
			if(!doc.exists("_rev")){
				ret["error"] = "Document already exists and no _rev was given.";
				bOk = false;
			} else {
				document oldDoc = getDocument(doc["_id"].string()); // TODO, do this without getDocument to make it faster
				if(doc["_rev"] != oldDoc["_rev"]){
					ret["error"] = "Document already exists and _rev given is not up to date.";
					bOk = false;
				}
			}
		} else {
			doc["_id"] = generateUUID();
		}
		if(bOk){
			doc["_rev"] = generateUUID();
			size_t lRevs = data["data"][doc["_id"].string()]["revs"].size();
			data["data"][doc["_id"].string()]["revs"].push_front(doc["_rev"].string());
			data["data"][doc["_id"].string()]["docs"][doc["_rev"].string()] = doc;
			data["data"][doc["_id"].string()]["sequence"] = data["sequence"];
			data["sequenceIndex"][data["sequence"].integer()]["_id"] = doc["_id"].string();
			data["sequenceIndex"][data["sequence"].integer()]["_rev"] = doc["_rev"].string();
			data["sequence"] = data["sequence"] + 1;
			size_t lMax = (size_t)data["config"]["maxRevisions"].integer();
			if(lMax > 0){
				for(size_t lIndex = lMax; lIndex < lRevs; lIndex++){
					data["data"][doc["_id"].string()]["docs"].erase(data["data"][doc["_id"].string()]["revs"].pop_back().string());
				}
			}
			ret["_id"] = doc["_id"];
			ret["_rev"] = doc["_rev"];
			if(data["config"]["autoSave"].boolean()){
				ret["save"] = save();
			}
		}
		mtx.unlock();
		return ret;
	}

	document database::addView(std::string sSetName, std::string sSetVersion, MAPPTR setMap, REDUCEPTR setReduce)
	{
		mtx.lock();
		bool bReIndex = false;
		if(data["views"][sSetName]["version"] != sSetVersion){
			bReIndex = true;
		}
		views[sSetName].map = setMap;
		views[sSetName].reduce = setReduce;
		data["views"][sSetName]["version"] = sSetVersion;
		document ret;
		if(bReIndex){
			data["indeces"].erase(sSetName);
			if(data["config"]["autoSave"].boolean()){
				ret["save"] = save();
			}
		}
		mtx.unlock();
		return ret;
	}

	bool database::viewSort(json::atom a, json::atom b)
	{
		return (a["key"] > b["key"]);
	}

	size_t database::matchLevel(atom& keys, atom& mappedResult)
	{
		if(keys.isA(JSON_ARRAY) && mappedResult["key"].isA(JSON_ARRAY)){
			size_t l = keys.size();
			size_t ret = 0;
			for(; ret < l; ret++){
				if(keys[ret] != mappedResult["key"][ret]){
					break;
				}
			}
			return ret;
		} else if(keys == mappedResult["key"]){
			return 1;
		} else {
			return 0;
		}
	}

	document database::getView(std::string sName, document keys, bool bReduce)
	{
		mtx.lock();
		if(views.find(sName) == views.end()){
			mtx.unlock();
			return document();
		}
		document ret;
		if(keys.exists("key")){
			getViewWorker(ret, sName, keys["key"], bReduce);
		} else if(keys.exists("keys")){
			size_t i = 0;
			for(iterator it = keys["keys"].begin(); it != keys["keys"].end(); ++it){
				getViewWorker(ret[i++], sName, (*it), bReduce);
			}
		} else {
			getViewWorker(ret, sName, keys, bReduce);
		}
		return ret;

	}

	atom & database::getViewWorker(atom & ret, std::string & sName, atom & keys, bool bReduce)
	{
		std::string sKeys = document(keys).write();
		indexView(sName, sKeys, keys);
		
		ret["total_rows"] = data["indeces"][sName][sKeys]["data"].size();
		ret["rows"] = data["indeces"][sName][sKeys]["data"];

		if(bReduce && views[sName].reduce){
			document values;
			values.emptyArray();
			for(iterator it = ret["rows"].begin(); it != ret["rows"].end(); ++it){
				values.push_back((*it)["value"]);
			}
			ret["rows"].clear();
			ret["rows"][0]["value"] = views[sName].reduce(keys, values, false);
			ret["rows"][0]["key"] = (char*)NULL;
			ret["total_rows"] = ret["rows"].size();
		}
		ret["name"] = sName;
		mtx.unlock();
		return ret;
	}


	document database::indexView(std::string &sName, std::string &sKeys, json::document keys)
	{
		mtx.lock();
		document ret;
		if(views.find(sName) != views.end()){
			if(views[sName].map){
				data["indeces"][sName][sKeys]["data"];
				iterator itIndex = data["indeces"][sName][sKeys].find("data");
				size_t sequence = data["indeces"][sName][sKeys]["sequence"].integer();
				size_t latest = data["sequence"].integer();
				data["indeces"][sName][sKeys]["sequence"] = latest;
				document newChanges;
				size_t keysSize = keys.size();
				for(size_t i = sequence + 1; i < latest; i++){
					std::string id = data["sequenceIndex"][i]["_id"].string();
					std::string rev = data["sequenceIndex"][i]["_rev"].string();
					if(data["data"][id]["sequence"] == i){ // skip out of date documents
						document ret = views[sName].map(data["data"][id]["docs"][rev]);
						if(!ret.empty()){
							if(!ret["key"].isA(JSON_OBJECT)){
								if(matchLevel(keys, ret) >= keysSize){
									document index;
									index["id"] = id;
									
									index["key"] = ret["key"];
									index["value"] = ret["value"];
									newChanges.push_back(index);
								}
							}
						}
					}
				}
				if(newChanges.empty()){
					mtx.unlock();
					return ret;
				}

				if((*itIndex).empty()){
					(*itIndex) = newChanges;
					if(data["config"]["autoSave"].boolean()){
						ret["save"] = save();
					}
					mtx.unlock();
					return ret;
				}

				newChanges.sort(&viewSort);
				iterator itNew = newChanges.begin();
				if(itNew != newChanges.end()){
					size_t l = (*itIndex).size();
					for(size_t i = 0; i < l; i++){
						if((*itIndex)[i]["id"] == (*itNew)["id"]){
							(*itIndex).erase(i);
							l--;
							++itNew;
							if(itNew == newChanges.end()){
								break;
							}
						}
					}
					itNew = newChanges.begin();
					for(size_t i = 0; i < l; i++){
						if((*itIndex)[i]["key"] > (*itNew)["key"]){
							(*itIndex).insert(i, (*itNew));
							l++;
							++itNew;
							if(itNew == newChanges.end()){
								break;
							}
						}
					}
					for(;itNew != newChanges.end(); ++itNew){
						(*itIndex).push_back((*itNew));
					}
					if(data["config"]["autoSave"].boolean()){
						ret["save"] = save();
					}
				}				
			}
		}
		mtx.unlock();
		return ret;
	}
	
	document database::cleanUpViews()
	{
		mtx.lock();
		data["indeces"].clear();
		document ret;
		if(data["config"]["autoSave"].boolean()){
			ret["save"] = save();
		}
		mtx.unlock();
		return ret;
	}
}

