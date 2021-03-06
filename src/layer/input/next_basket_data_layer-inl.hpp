#ifndef TEXTNET_LAYER_NEXTBASKETDATA_LAYER_INL_HPP_
#define TEXTNET_LAYER_NEXTBASKETDATA_LAYER_INL_HPP_

#include <iostream>
#include <fstream>
#include <sstream>

#include <mshadow/tensor.h>
#include "../layer.h"
#include "../op.h"
#include "../../utils/random.h"

namespace textnet {
namespace layer {

template<typename xpu>
class NextBasketDataLayer : public Layer<xpu>{
 public:
  NextBasketDataLayer(LayerType type) { this->layer_type = type; mul = 1000;}
  virtual ~NextBasketDataLayer(void) {}
  
  virtual int BottomNodeNum() { return 0; }
  virtual int TopNodeNum() { return 5; }
  virtual int ParamNodeNum() { return 0; }

  virtual void Require() {
    // default value, just set the value you want
    this->defaults["shuffle_seed"] = SettingV(123);
    // require value, set to SettingV(),
    // it will force custom to set in config
    this->defaults["data_file"] = SettingV();
    this->defaults["batch_size"] = SettingV();
    this->defaults["max_session_len"] = SettingV();
    this->defaults["max_context_len"] = SettingV();
    this->defaults["train_or_pred"] = SettingV();
    
    Layer<xpu>::Require();
  }

  
  virtual void SetupLayer(std::map<std::string, SettingV> &setting,
                          const std::vector<Node<xpu>*> &bottom,
                          const std::vector<Node<xpu>*> &top,
                          mshadow::Random<xpu> *prnd) {
    Layer<xpu>::SetupLayer(setting, bottom, top, prnd);

    max_context_len = setting["max_context_len"].i_val;
    max_session_len = setting["max_session_len"].i_val;
    data_file       = setting["data_file"].s_val;
    batch_size      = setting["batch_size"].i_val;
    train_or_pred   = setting["train_or_pred"].s_val;
    
    utils::Check(bottom.size() == BottomNodeNum(), "NextBasketDataLayer:bottom size problem."); 
    utils::Check(top.size() == TopNodeNum(), "NextBasketDataLayer:top size problem.");
                  
    ReadNextBasketData();
    example_ptr = 0;
    test_example_ptr = 0;
    sampler.Seed(shuffle_seed);
  }

  void splitByChar(const std::string &s, char c, std::vector<std::string> &vsResult)
  {
      using std::string;
      vsResult.clear();
      size_t uPos = 0, uPrePos = 0;
      string sTmp;
      for (; uPos < s.size(); uPos++)
      {
          if (s[uPos] == c)
          {
              sTmp = s.substr(uPrePos, uPos-uPrePos);
              if (!sTmp.empty())
                  vsResult.push_back(sTmp);
              uPrePos = uPos + 1;
          }
      }
      if (uPrePos < s.size())
      {
          sTmp = s.substr(uPrePos);
          vsResult.push_back(sTmp);
      }
  }
  
  void ReadNextBasketData() {
    utils::Printf("Open data file: %s\n", data_file.c_str());	
    std::vector<std::string> lines;
    std::ifstream fin(data_file.c_str());
    std::string s;
    utils::Check(fin.is_open(), "Open data file problem.");
    while (!fin.eof()) {
      std::getline(fin, s);
      if (s.empty()) break;
      lines.push_back(s);
    }
    fin.close();
    
    line_count = lines.size();
	utils::Printf("Line count in file: %d\n", line_count);

    std::istringstream iss;
    for (int i = 0; i < line_count; ++i) {
        std::vector<std::string> vsTab, vsComma;
        Example e;
        splitByChar(lines[i], ' ', vsTab); 
        e.user = atoi(vsTab[1].c_str());
        splitByChar(vsTab[0], ',', vsComma); 
        for (int j = 0; j < vsComma.size(); ++j) {
            e.next_items.push_back(atoi(vsComma[j].c_str()));
        }
        utils::Check(vsTab.size() <= max_context_len + 2, "NextBasketDataLayer: input data error.");
        for (int k = 2; k < vsTab.size(); ++k) {
            std::vector<int> basket;
            splitByChar(vsTab[k], ',', vsComma); 
            for (int j = 0; j < vsComma.size(); ++j) {
                basket.push_back(atoi(vsComma[j].c_str()));
            }
            e.context.push_back(basket);
        }
        dataset.push_back(e);
    }
	utils::Printf("Example count in file: %d\n", dataset.size());

    // gen example ids
    for (int i = 0; i < dataset.size(); ++i) {
      for (int j = 0; j < dataset[i].next_items.size(); ++j) {
        example_ids.push_back(i * mul + j);
      }
    }
  }
  
  virtual void Reshape(const std::vector<Node<xpu>*> &bottom,
                       const std::vector<Node<xpu>*> &top,
					   bool show_info = false) {
    utils::Check(bottom.size() == BottomNodeNum(), "NextBasketDataLayer:bottom size problem."); 
    utils::Check(top.size() == TopNodeNum(), "NextBasketDataLayer:top size problem.");

    top[0]->Resize(batch_size, 1, 1, 1, true); // user id 
    top[1]->Resize(batch_size, max_context_len, 1, max_session_len, true); // context 
    top[2]->Resize(batch_size, 1, 1, 1, true); // context length
    top[3]->Resize(batch_size, 1, 1, 1, true); // y for train
    top[4]->Resize(batch_size, 1, 1, max_session_len, true); // ys for eval
	if (show_info) {
		top[0]->PrintShape("top0");
		top[1]->PrintShape("top1");
		top[2]->PrintShape("top2");
		top[3]->PrintShape("top3");
		top[4]->PrintShape("top4");
	}
		
  }
  
  virtual void Forward(const std::vector<Node<xpu>*> &bottom,
                       const std::vector<Node<xpu>*> &top) {
    for (int node_idx = 0; node_idx < top.size(); ++node_idx) {
      top[node_idx]->data = 0;
      top[node_idx]->length = 0;
    }

    for (int i = 0; i < batch_size; ++i) {
      int exampleIdx = 0, labelIdx = 0;
      if (train_or_pred=="train") {
        if (example_ptr == 0) {
          this->sampler.Shuffle(example_ids);
        }
        exampleIdx = example_ids[example_ptr] / mul;
        labelIdx   = example_ids[example_ptr] % mul;
        example_ptr = (example_ptr + 1) % example_ids.size();
      } else {
        exampleIdx = test_example_ptr;
        labelIdx   = 0;
        test_example_ptr = (test_example_ptr + 1) % dataset.size();
      }
         
      top[3]->data[i][0][0][0] = dataset[exampleIdx].next_items[labelIdx]; // y
      for (int k = 0; k < dataset[exampleIdx].next_items.size(); ++k) {
        top[4]->data[i][0][0][k] = dataset[exampleIdx].next_items[k]; // ys 
      }
      top[4]->length[i][0] = dataset[exampleIdx].next_items.size();
      top[0]->data[i][0][0][0] = dataset[exampleIdx].user; // user
      top[0]->length[i][0] = 1; // embed is a var len layer
      for (int w = 0; w < dataset[exampleIdx].context.size(); ++w) {
        top[2]->data[i][0][0][0] = dataset[exampleIdx].context.size();
        for (int k = 0; k < dataset[exampleIdx].context[w].size(); ++k) {
          top[1]->data[i][w][0][k] = dataset[exampleIdx].context[w][k];
        }
        top[1]->length[i][w] = dataset[exampleIdx].context[w].size();
      }
    }
  }
  
  virtual void Backprop(const std::vector<Node<xpu>*> &bottom,
                        const std::vector<Node<xpu>*> &top) {
    // nothing to do
  }

  struct Example {
      int user;
      std::vector<int> next_items;
      std::vector<std::vector<int> > context;
  };


 protected:
  std::string data_file, train_or_pred;
  int batch_size, max_context_len, max_session_len, example_ptr, mul, test_example_ptr;

  std::vector<Example> dataset;
  
  std::vector<int> example_ids;
  int line_count;
  int shuffle_seed;
  utils::RandomSampler sampler;
};
}  // namespace layer
}  // namespace textnet
#endif 

